#include "ota_cloud.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ota_flash.h"
#include "ota_parse.h"
#include "lte.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ota_cloud";

#define NVS_NS "elm"
/*
 * Persistent OTA configuration (stored in ESP-IDF NVS partition).
 *
 * Why in NVS (not ota_0/ota_1)?
 * - OTA overwrites only the inactive app slot.
 * - NVS survives app upgrades so the device keeps the same LTE host/APN/device_id.
 *
 * Keys:
 * - ota_manif: firmware-check POST URL (overrides Kconfig CHECK_URL when non-empty)
 * - ota_chan:  retained for NVS/UI compatibility (not appended to URL)
 * - ota_force: if true, reflash even when version matches
 * - uplink_did: uplink/device_id; kept in sync with OTA "device_id"
 * - ota_applied: last successfully applied latestVersion (promoted only after
 *                 boot verify — prevents lab label-bumps / rollback retry blocks)
 * - ota_pend:    latestVersion written just before reboot; promoted on confirm
 */
#define NVS_KEY_URL "ota_manif"
#define NVS_KEY_CHAN "ota_chan"
#define NVS_KEY_FORCE "ota_force"
#define NVS_KEY_DID "uplink_did"
#define NVS_KEY_TOKEN "ota_token"
#define NVS_KEY_USER "ota_user"
#define NVS_KEY_APPLIED "ota_applied"
#define NVS_KEY_PENDING "ota_pend"

#define CHECK_RESP_BUF_LEN 4096

/* Presigned S3 GET can fail transiently (UART glitch, brief LTE stall). */
#define OTA_CLOUD_STREAM_ATTEMPTS 3
#define OTA_CLOUD_STREAM_RETRY_MS 3000

#ifndef CONFIG_OTA_CLOUD_CHECK_URL
#define CONFIG_OTA_CLOUD_CHECK_URL ""
#endif
#ifndef CONFIG_OTA_CLOUD_AUTH_HEADER
#define CONFIG_OTA_CLOUD_AUTH_HEADER ""
#endif
#ifndef CONFIG_OTA_CLOUD_SYSTEM_USER_ID
#define CONFIG_OTA_CLOUD_SYSTEM_USER_ID ""
#endif
#ifndef CONFIG_OTA_CLOUD_MANUFACTURER
#define CONFIG_OTA_CLOUD_MANUFACTURER "Espressif Systems"
#endif
#ifndef CONFIG_OTA_CLOUD_DEVICE_TYPE
#define CONFIG_OTA_CLOUD_DEVICE_TYPE "fleet monitor"
#endif
#ifndef CONFIG_OTA_CLOUD_AUTO_WAIT_SEC
#define CONFIG_OTA_CLOUD_AUTO_WAIT_SEC 90
#endif
#ifndef CONFIG_OTA_CLOUD_AUTO_INTERVAL_MIN
#ifdef CONFIG_OTA_CLOUD_AUTO_INTERVAL_H
#define CONFIG_OTA_CLOUD_AUTO_INTERVAL_MIN (CONFIG_OTA_CLOUD_AUTO_INTERVAL_H * 60)
#else
#define CONFIG_OTA_CLOUD_AUTO_INTERVAL_MIN 30
#endif
#endif

static SemaphoreHandle_t s_mu;
static ota_cloud_config_t s_cfg;
static ota_cloud_status_t s_st;
static TaskHandle_t s_task;
static TaskHandle_t s_auto_task;
static char s_applied[40];

static esp_err_t save_nvs(const ota_cloud_config_t *c);

/** @brief Update phase/error under caller-held s_mu. */
static void set_phase(ota_cloud_phase_t p, const char *err)
{
    s_st.phase = p;
    if (err) {
        snprintf(s_st.error, sizeof(s_st.error), "%s", err);
    } else if (p != OTA_CLOUD_FAILED) {
        s_st.error[0] = '\0';
    }
}

/** @brief Always set firmware-check URL from Kconfig (fleet-wide, bin only). */
static void apply_default_url(ota_cloud_config_t *c)
{
    const char *def = CONFIG_OTA_CLOUD_CHECK_URL;
    if (def && def[0]) {
        snprintf(c->manifest_url, sizeof(c->manifest_url), "%s", def);
    } else {
        c->manifest_url[0] = '\0';
    }
}

/** @brief Defaults; device_id starts as fleet-demo-001 (override via USB). */
static void defaults(ota_cloud_config_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->channel, sizeof(c->channel), "%s", "stable");
    snprintf(c->device_id, sizeof(c->device_id), "%s", "fleet-demo-001");
    c->force = false;
    apply_default_url(c);
    if (CONFIG_OTA_CLOUD_AUTH_HEADER[0]) {
        snprintf(c->auth_token, sizeof(c->auth_token), "%s", CONFIG_OTA_CLOUD_AUTH_HEADER);
    }
    if (CONFIG_OTA_CLOUD_SYSTEM_USER_ID[0]) {
        snprintf(c->system_user_id, sizeof(c->system_user_id), "%s",
                 CONFIG_OTA_CLOUD_SYSTEM_USER_ID);
    }
}

/**
 * @brief Load IDs/tokens from NVS; OTA check URL always from bin Kconfig.
 * @note Erases legacy ota_manif URL key so old lab overrides cannot stick.
 */
static esp_err_t load_nvs(void)
{
    defaults(&s_cfg);
    s_applied[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_OK;
    }
    size_t len = sizeof(s_cfg.channel);
    nvs_get_str(h, NVS_KEY_CHAN, s_cfg.channel, &len);
    len = sizeof(s_cfg.device_id);
    nvs_get_str(h, NVS_KEY_DID, s_cfg.device_id, &len);
    len = sizeof(s_cfg.auth_token);
    nvs_get_str(h, NVS_KEY_TOKEN, s_cfg.auth_token, &len);
    len = sizeof(s_cfg.system_user_id);
    nvs_get_str(h, NVS_KEY_USER, s_cfg.system_user_id, &len);
    len = sizeof(s_applied);
    nvs_get_str(h, NVS_KEY_APPLIED, s_applied, &len);
    uint8_t force = 0;
    if (nvs_get_u8(h, NVS_KEY_FORCE, &force) == ESP_OK) {
        s_cfg.force = force != 0;
    }
    if (nvs_erase_key(h, NVS_KEY_URL) == ESP_OK) {
        (void)nvs_commit(h);
        ESP_LOGI(TAG, "cleared legacy NVS OTA check URL (using bin CONFIG_OTA_CLOUD_CHECK_URL)");
    }
    nvs_close(h);
    apply_default_url(&s_cfg);
    if (s_cfg.channel[0] == '\0') {
        snprintf(s_cfg.channel, sizeof(s_cfg.channel), "%s", "stable");
    }
    return ESP_OK;
}

/**
 * @brief Persist ota_applied + RAM s_applied.
 * @warning Call only after successful boot verify (today called pre-reboot — B1).
 */
/**
 * @brief Persist ota_applied + RAM s_applied (only after boot verify).
 */
static esp_err_t save_applied_version(const char *version)
{
    if (!version || !version[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_APPLIED, version);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        snprintf(s_applied, sizeof(s_applied), "%s", version);
    }
    return err;
}

/**
 * @brief Store Trafyn latestVersion as ota_pend before reboot (not yet applied).
 */
static esp_err_t save_pending_version(const char *version)
{
    if (!version || !version[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_PENDING, version);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/** @brief Erase ota_pend (rollback or failed end_and_reboot). */
static esp_err_t clear_pending_version(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, NVS_KEY_PENDING);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/** @brief Persist channel/force/device_id/tokens (not check URL — that is bin-only). */
static esp_err_t save_nvs(const ota_cloud_config_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    /* Drop legacy URL key if present; never persist check URL. */
    (void)nvs_erase_key(h, NVS_KEY_URL);
    err = nvs_set_str(h, NVS_KEY_CHAN, c->channel);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_FORCE, c->force ? 1 : 0);
    }
    /* device_id: keep uplink key in sync when provided */
    if (err == ESP_OK && c->device_id[0]) {
        err = nvs_set_str(h, NVS_KEY_DID, c->device_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_TOKEN, c->auth_token);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_USER, c->system_user_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t ota_cloud_init(void)
{
    if (s_mu == NULL) {
        s_mu = xSemaphoreCreateMutex();
        if (!s_mu) {
            return ESP_ERR_NO_MEM;
        }
    }
    load_nvs();
    /*
     * Drop orphaned ota_pend when this boot is not pending verify (rollback or
     * leftover). If we ARE pending verify, keep pend for commit after SoftAP.
     */
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t img_state = ESP_OTA_IMG_UNDEFINED;
        if (running &&
            esp_ota_get_state_partition(running, &img_state) == ESP_OK &&
            img_state != ESP_OTA_IMG_PENDING_VERIFY) {
            (void)clear_pending_version();
        }
    }
    memset(&s_st, 0, sizeof(s_st));
    set_phase(OTA_CLOUD_IDLE, NULL);
    ESP_LOGI(TAG, "init url='%s' channel='%s' force=%d", s_cfg.manifest_url, s_cfg.channel,
             (int)s_cfg.force);
    return ESP_OK;
}

esp_err_t ota_cloud_get_config(ota_cloud_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mu);
    return ESP_OK;
}

esp_err_t ota_cloud_set_config(const ota_cloud_config_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_cfg = *in;
    if (s_cfg.channel[0] == '\0') {
        snprintf(s_cfg.channel, sizeof(s_cfg.channel), "%s", "stable");
    }
    /* Ignore any caller-supplied check URL; fleet endpoint is from the bin. */
    apply_default_url(&s_cfg);
    esp_err_t err = save_nvs(&s_cfg);
    xSemaphoreGive(s_mu);
    return err;
}

esp_err_t ota_cloud_get_status(ota_cloud_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *out = s_st;
    snprintf(out->applied_version, sizeof(out->applied_version), "%s", s_applied);
    xSemaphoreGive(s_mu);
    return ESP_OK;
}

/** @brief Stream chunk → ota_flash_write; bump bytes_downloaded under mutex. */
static esp_err_t stream_write_cb(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    esp_err_t err = ota_flash_write(data, len);
    if (err == ESP_OK) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_st.bytes_downloaded += len;
        xSemaphoreGive(s_mu);
    }
    return err;
}

esp_err_t ota_cloud_run(void)
{
    ota_cloud_config_t cfg;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    cfg = s_cfg;
    s_st.bytes_downloaded = 0;
    s_st.http_status = 0;
    s_st.update_available = false;
    s_st.manifest_version[0] = '\0';
    s_st.current_version[0] = '\0';
    set_phase(OTA_CLOUD_CHECKING, NULL);
    xSemaphoreGive(s_mu);

    if (cfg.manifest_url[0] == '\0') {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(OTA_CLOUD_FAILED, "check_url empty");
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg.device_id[0] == '\0') {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(OTA_CLOUD_FAILED, "device_id not provisioned");
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * OTA algorithm (LTE / Trafyn path):
     *
     * 1) Read running app version as-is; POST firmware-check JSON to check URL.
     *    (No stripping: comparisons below use the full version string, so a
     *    rebuild with only a different build-number suffix still counts as
     *    a real mismatch/update — see components/ota/ota_parse.c.)
     * 2) Parse Trafyn envelope via ota_parse_check_json.
     * 3) FAIL / NO_UPDATE → set phase and return.
     * 4) UPDATE: skip if !force and latestVersion == ota_applied; else
     *    ota_flash_begin → stream presigned URL → save_applied → reboot.
     */
    char current_ver[32];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(current_ver, sizeof(current_ver), "%s", app && app->version[0] ? app->version : "");

    xSemaphoreTake(s_mu, portMAX_DELAY);
    snprintf(s_st.current_version, sizeof(s_st.current_version), "%s", current_ver);
    xSemaphoreGive(s_mu);

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(OTA_CLOUD_FAILED, "no_mem");
        xSemaphoreGive(s_mu);
        return ESP_ERR_NO_MEM;
    }
    cJSON *input = cJSON_AddObjectToObject(body, "input");
    if (!input) {
        cJSON_Delete(body);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(OTA_CLOUD_FAILED, "no_mem");
        xSemaphoreGive(s_mu);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(input, "deviceId", cfg.device_id);
    cJSON_AddStringToObject(input, "manufacturer", CONFIG_OTA_CLOUD_MANUFACTURER);
    cJSON_AddStringToObject(input, "deviceType", CONFIG_OTA_CLOUD_DEVICE_TYPE);
    cJSON_AddStringToObject(input, "currentVersion", current_ver[0] ? current_ver : "0.0.0");
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!payload) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(OTA_CLOUD_FAILED, "no_mem");
        xSemaphoreGive(s_mu);
        return ESP_ERR_NO_MEM;
    }

    lte_http_req_headers_t hdr = {
        .authorization = cfg.auth_token[0] ? cfg.auth_token : NULL,
        .system_user_id = cfg.system_user_id[0] ? cfg.system_user_id : NULL,
    };

    char *resp = malloc(CHECK_RESP_BUF_LEN);
    if (!resp) {
        free(payload);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(OTA_CLOUD_FAILED, "no_mem");
        xSemaphoreGive(s_mu);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "firmware-check POST: %s (currentVersion=%s)", cfg.manifest_url,
             current_ver[0] ? current_ver : "0.0.0");

    lte_http_result_t hr;
    memset(&hr, 0, sizeof(hr));
    size_t got = 0;
    esp_err_t err = lte_http_post_recv(cfg.manifest_url, payload, &hdr, resp, CHECK_RESP_BUF_LEN,
                                          &got, &hr);
    free(payload);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.http_status = hr.http_status;
    xSemaphoreGive(s_mu);

    if (err != ESP_OK) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(OTA_CLOUD_FAILED, hr.error[0] ? hr.error : "check_post_fail");
        xSemaphoreGive(s_mu);
        free(resp);
        return err;
    }

    ota_check_result_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    if (ota_parse_check_json(resp, current_ver, &parsed) != 0) {
        free(resp);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(OTA_CLOUD_FAILED, "parse_error");
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_RESPONSE;
    }
    free(resp);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.update_available = parsed.update_available;
    if (parsed.latest_version[0]) {
        snprintf(s_st.manifest_version, sizeof(s_st.manifest_version), "%s", parsed.latest_version);
    }
    xSemaphoreGive(s_mu);

    if (parsed.kind == OTA_CHECK_FAIL) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(OTA_CLOUD_FAILED, parsed.error[0] ? parsed.error : "check_fail");
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (parsed.kind == OTA_CHECK_NO_UPDATE) {
        ESP_LOGI(TAG, "no_update (updateAvailable=%d latest='%s')", (int)parsed.update_available,
                 parsed.latest_version);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(OTA_CLOUD_NO_UPDATE, NULL);
        xSemaphoreGive(s_mu);
        return ESP_OK;
    }

    /* OTA_CHECK_UPDATE */
    if (!cfg.force && s_applied[0] && strcmp(s_applied, parsed.latest_version) == 0) {
        ESP_LOGI(TAG, "latest %s already applied — no_update", parsed.latest_version);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(OTA_CLOUD_NO_UPDATE, NULL);
        xSemaphoreGive(s_mu);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "update -> %s size=%u", parsed.latest_version, (unsigned)parsed.size);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    set_phase(OTA_CLOUD_DOWNLOADING, NULL);
    xSemaphoreGive(s_mu);

    /*
     * Stream the binary into ota_flash_write() (no full-.bin RAM buffer).
     * Retry a few times on stream/read fail so trucks recover without SoftAP.
     * GPS background AT is paused by lte_ota_task for this whole worker.
     */
    err = ESP_FAIL;
    for (int attempt = 1; attempt <= OTA_CLOUD_STREAM_ATTEMPTS; ++attempt) {
        err = ota_flash_begin(parsed.size, parsed.sha256);
        if (err != ESP_OK) {
            ota_flash_status_t ost;
            ota_flash_get_status(&ost);
            xSemaphoreTake(s_mu, portMAX_DELAY);
            set_phase(OTA_CLOUD_FAILED, ost.error[0] ? ost.error : "ota_flash_begin");
            xSemaphoreGive(s_mu);
            return err;
        }

        size_t clen = 0;
        memset(&hr, 0, sizeof(hr));
        ESP_LOGI(TAG, "bin stream attempt %d/%d", attempt, OTA_CLOUD_STREAM_ATTEMPTS);
        err = lte_http_get_stream(parsed.presigned_url, stream_write_cb, NULL, &clen, &hr);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_st.http_status = hr.http_status;
        xSemaphoreGive(s_mu);

        if (err == ESP_OK) {
            break;
        }

        ESP_LOGW(TAG, "bin stream fail attempt %d/%d: %s", attempt, OTA_CLOUD_STREAM_ATTEMPTS,
                 hr.error[0] ? hr.error : esp_err_to_name(err));
        ota_flash_abort();
        if (attempt < OTA_CLOUD_STREAM_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(OTA_CLOUD_STREAM_RETRY_MS));
        }
    }

    char pending_ver[40];
    snprintf(pending_ver, sizeof(pending_ver), "%s", parsed.latest_version);

    if (err != ESP_OK) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(OTA_CLOUD_FAILED, hr.error[0] ? hr.error : "bin_download_fail");
        xSemaphoreGive(s_mu);
        return err;
    }

    /* Persist as pending only — promote to ota_applied after boot verify (B1). */
    (void)save_pending_version(pending_ver);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    set_phase(OTA_CLOUD_REBOOTING, NULL);
    xSemaphoreGive(s_mu);

    err = ota_flash_end_and_reboot();
    /* only on failure — drop pending so a rollback can redownload */
    (void)clear_pending_version();
    ota_flash_abort();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    set_phase(OTA_CLOUD_FAILED, "end_reboot_fail");
    xSemaphoreGive(s_mu);
    return err;
}

/** @brief Worker: suspend BG AT, ota_cloud_run, resume, clear s_task. */
static void lte_ota_task(void *arg)
{
    (void)arg;
    /* Also cover firmware-check POST: no GPS AT while OTA worker runs. */
    lte_suspend_bg_at(true);
    ota_cloud_run();
    lte_suspend_bg_at(false);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t ota_cloud_start_background(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ota_flash_is_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(lte_ota_task, "ota_cloud", 8192, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool ota_cloud_is_busy(void)
{
    ota_cloud_status_t st;
    if (ota_cloud_get_status(&st) != ESP_OK) {
        return false;
    }
    return st.phase == OTA_CLOUD_CHECKING || st.phase == OTA_CLOUD_DOWNLOADING ||
           st.phase == OTA_CLOUD_REBOOTING || s_task != NULL;
}

esp_err_t ota_cloud_commit_pending_applied(void)
{
    char pending[40];
    pending[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK;
    }
    size_t len = sizeof(pending);
    (void)nvs_get_str(h, NVS_KEY_PENDING, pending, &len);
    nvs_close(h);
    if (pending[0] == '\0') {
        return ESP_OK;
    }

    esp_err_t err = save_applied_version(pending);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "commit pending→applied failed: %s", esp_err_to_name(err));
        return err;
    }
    (void)clear_pending_version();
    ESP_LOGI(TAG, "committed ota_applied='%s' after boot verify", pending);
    return ESP_OK;
}

#if CONFIG_OTA_CLOUD_AUTO_CHECK

/** @brief Poll lte_refresh until registered or timeout. */
static bool wait_lte_registered(int max_sec)
{
    for (int i = 0; i < max_sec; i++) {
        (void)lte_refresh();
        lte_status_t st;
        if (lte_get_status(&st) == ESP_OK && st.enabled && st.sim_ready && st.registered) {
            ESP_LOGI(TAG, "auto: LTE registered (waited %ds)", i);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "auto: LTE not registered after %ds — trying check anyway", max_sec);
    return false;
}

/**
 * @brief Ensure URL; if force set, clear and persist (see O2 — prefer local-only clear).
 */
static void ensure_auto_config(void)
{
    ota_cloud_config_t cfg;
    if (ota_cloud_get_config(&cfg) != ESP_OK) {
        return;
    }
    /* Check URL always from bin; auto path never force-reflashes. */
    apply_default_url(&cfg);
    if (cfg.force) {
        cfg.force = false;
        (void)ota_cloud_set_config(&cfg);
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    apply_default_url(&s_cfg);
    xSemaphoreGive(s_mu);
}

/** @brief One auto cycle: start_background and wait until not busy. */
static void auto_check_once(void)
{
    ensure_auto_config();
    ota_cloud_config_t cfg;
    if (ota_cloud_get_config(&cfg) != ESP_OK || cfg.manifest_url[0] == '\0') {
        ESP_LOGW(TAG, "auto: no check URL — skip");
        return;
    }
    if (cfg.device_id[0] == '\0') {
        ESP_LOGW(TAG, "auto: device_id not provisioned — skip");
        return;
    }
    if (ota_cloud_is_busy() || ota_flash_is_busy()) {
        ESP_LOGW(TAG, "auto: OTA busy — skip this cycle");
        return;
    }
    ESP_LOGI(TAG, "auto: starting OTA check url='%s'", cfg.manifest_url);
    esp_err_t err = ota_cloud_start_background();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "auto: start failed: %s", esp_err_to_name(err));
        return;
    }
    /* Wait until worker finishes (reboot never returns) */
    while (ota_cloud_is_busy()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/** @brief Infinite auto-check loop with interval sleep. */
static void lte_ota_auto_task(void *arg)
{
    (void)arg;
    const int wait_sec = CONFIG_OTA_CLOUD_AUTO_WAIT_SEC;
    const int interval_min = CONFIG_OTA_CLOUD_AUTO_INTERVAL_MIN > 0
                                 ? CONFIG_OTA_CLOUD_AUTO_INTERVAL_MIN
                                 : 30;
    ESP_LOGI(TAG, "auto: task started (wait=%ds interval=%dmin)", wait_sec, interval_min);

    for (;;) {
        wait_lte_registered(wait_sec);
        auto_check_once();
        ESP_LOGI(TAG, "auto: next check in %d minute(s)", interval_min);
        vTaskDelay(pdMS_TO_TICKS((uint32_t)interval_min * 60U * 1000U));
    }
}

esp_err_t ota_cloud_start_auto(void)
{
    if (s_auto_task != NULL) {
        return ESP_OK;
    }
    if (xTaskCreate(lte_ota_auto_task, "ota_auto", 4096, NULL, 4, &s_auto_task) != pdPASS) {
        s_auto_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else /* !CONFIG_OTA_CLOUD_AUTO_CHECK */

esp_err_t ota_cloud_start_auto(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
