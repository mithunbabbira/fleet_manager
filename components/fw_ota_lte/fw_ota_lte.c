#include "fw_ota_lte.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fw_ota.h"
#include "fw_ota_lte_parse.h"
#include "net_lte.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "fw_ota_lte";

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
#define NVS_KEY_APPLIED "ota_applied"
#define NVS_KEY_PENDING "ota_pend"

#define CHECK_RESP_BUF_LEN 4096

/* Presigned S3 GET can fail transiently (UART glitch, brief LTE stall). */
#define FW_OTA_LTE_STREAM_ATTEMPTS 3
#define FW_OTA_LTE_STREAM_RETRY_MS 3000

#ifndef CONFIG_FW_OTA_LTE_CHECK_URL
#define CONFIG_FW_OTA_LTE_CHECK_URL ""
#endif
#ifndef CONFIG_FW_OTA_LTE_AUTH_HEADER
#define CONFIG_FW_OTA_LTE_AUTH_HEADER ""
#endif
#ifndef CONFIG_FW_OTA_LTE_SYSTEM_USER_ID
#define CONFIG_FW_OTA_LTE_SYSTEM_USER_ID ""
#endif
#ifndef CONFIG_FW_OTA_LTE_MANUFACTURER
#define CONFIG_FW_OTA_LTE_MANUFACTURER "Espressif Systems"
#endif
#ifndef CONFIG_FW_OTA_LTE_DEVICE_TYPE
#define CONFIG_FW_OTA_LTE_DEVICE_TYPE "fleet monitor"
#endif
#ifndef CONFIG_FW_OTA_LTE_AUTO_WAIT_SEC
#define CONFIG_FW_OTA_LTE_AUTO_WAIT_SEC 90
#endif
#ifndef CONFIG_FW_OTA_LTE_AUTO_INTERVAL_H
#define CONFIG_FW_OTA_LTE_AUTO_INTERVAL_H 24
#endif

static SemaphoreHandle_t s_mu;
static fw_ota_lte_config_t s_cfg;
static fw_ota_lte_status_t s_st;
static TaskHandle_t s_task;
static TaskHandle_t s_auto_task;
static char s_applied[40];

static esp_err_t save_nvs(const fw_ota_lte_config_t *c);

/** @brief Update phase/error under caller-held s_mu. */
static void set_phase(fw_ota_lte_phase_t p, const char *err)
{
    s_st.phase = p;
    if (err) {
        snprintf(s_st.error, sizeof(s_st.error), "%s", err);
    } else if (p != FW_OTA_LTE_FAILED) {
        s_st.error[0] = '\0';
    }
}

/** @brief Fill empty manifest_url from Kconfig CHECK_URL. */
static void apply_default_url(fw_ota_lte_config_t *c)
{
    if (c->manifest_url[0] != '\0') {
        return;
    }
    const char *def = CONFIG_FW_OTA_LTE_CHECK_URL;
    if (def && def[0]) {
        snprintf(c->manifest_url, sizeof(c->manifest_url), "%s", def);
    }
}

/** @brief Detect legacy ngrok / /firmware/manifest check URLs. */
static bool check_url_is_legacy_lab(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return false;
    }
    if (strstr(url, "ngrok") != NULL) {
        return true;
    }
    if (strstr(url, "/firmware/manifest") != NULL) {
        return true;
    }
    return false;
}

/** @brief Replace legacy URL with Kconfig default; return true if rewritten. */
static bool sanitize_check_url(fw_ota_lte_config_t *c)
{
    if (!check_url_is_legacy_lab(c->manifest_url)) {
        apply_default_url(c);
        return false;
    }
    ESP_LOGW(TAG, "ignoring legacy lab check URL (ngrok/GET manifest)");
    c->manifest_url[0] = '\0';
    apply_default_url(c);
    return true;
}

/** @brief Zero config; device_id must be provisioned via Carrier Console. */
static void defaults(fw_ota_lte_config_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->channel, sizeof(c->channel), "%s", "stable");
    c->force = false;
    c->manifest_url[0] = '\0';
    apply_default_url(c);
}

/** @brief Load NVS into s_cfg/s_applied; may rewrite legacy URL. */
static esp_err_t load_nvs(void)
{
    defaults(&s_cfg);
    s_cfg.manifest_url[0] = '\0';
    s_applied[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        apply_default_url(&s_cfg);
        return ESP_OK;
    }
    size_t len = sizeof(s_cfg.manifest_url);
    nvs_get_str(h, NVS_KEY_URL, s_cfg.manifest_url, &len);
    len = sizeof(s_cfg.channel);
    nvs_get_str(h, NVS_KEY_CHAN, s_cfg.channel, &len);
    len = sizeof(s_cfg.device_id);
    nvs_get_str(h, NVS_KEY_DID, s_cfg.device_id, &len);
    len = sizeof(s_applied);
    nvs_get_str(h, NVS_KEY_APPLIED, s_applied, &len);
    uint8_t force = 0;
    if (nvs_get_u8(h, NVS_KEY_FORCE, &force) == ESP_OK) {
        s_cfg.force = force != 0;
    }
    nvs_close(h);
    bool replaced = sanitize_check_url(&s_cfg);
    if (s_cfg.channel[0] == '\0') {
        snprintf(s_cfg.channel, sizeof(s_cfg.channel), "%s", "stable");
    }
    if (replaced) {
        (void)save_nvs(&s_cfg);
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

/** @brief Persist URL/channel/force/device_id (uplink_did). */
static esp_err_t save_nvs(const fw_ota_lte_config_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_URL, c->manifest_url);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_CHAN, c->channel);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_FORCE, c->force ? 1 : 0);
    }
    /* device_id: keep uplink key in sync when provided */
    if (err == ESP_OK && c->device_id[0]) {
        err = nvs_set_str(h, NVS_KEY_DID, c->device_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t fw_ota_lte_init(void)
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
    set_phase(FW_OTA_LTE_IDLE, NULL);
    ESP_LOGI(TAG, "init url='%s' channel='%s' force=%d", s_cfg.manifest_url, s_cfg.channel,
             (int)s_cfg.force);
    return ESP_OK;
}

esp_err_t fw_ota_lte_get_config(fw_ota_lte_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mu);
    return ESP_OK;
}

esp_err_t fw_ota_lte_set_config(const fw_ota_lte_config_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_cfg = *in;
    if (s_cfg.channel[0] == '\0') {
        snprintf(s_cfg.channel, sizeof(s_cfg.channel), "%s", "stable");
    }
    (void)sanitize_check_url(&s_cfg);
    esp_err_t err = save_nvs(&s_cfg);
    xSemaphoreGive(s_mu);
    return err;
}

esp_err_t fw_ota_lte_get_status(fw_ota_lte_status_t *out)
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

/** @brief Stream chunk → fw_ota_write; bump bytes_downloaded under mutex. */
static esp_err_t stream_write_cb(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    esp_err_t err = fw_ota_write(data, len);
    if (err == ESP_OK) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_st.bytes_downloaded += len;
        xSemaphoreGive(s_mu);
    }
    return err;
}

esp_err_t fw_ota_lte_run(void)
{
    fw_ota_lte_config_t cfg;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    cfg = s_cfg;
    s_st.bytes_downloaded = 0;
    s_st.http_status = 0;
    s_st.update_available = false;
    s_st.manifest_version[0] = '\0';
    s_st.current_version[0] = '\0';
    set_phase(FW_OTA_LTE_CHECKING, NULL);
    xSemaphoreGive(s_mu);

    if (cfg.manifest_url[0] == '\0') {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, "check_url empty");
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg.device_id[0] == '\0') {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, "device_id not provisioned");
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * OTA algorithm (LTE / Trafyn path):
     *
     * 1) Strip running app version; POST firmware-check JSON to check URL.
     * 2) Parse Trafyn envelope via fw_ota_parse_check_json.
     * 3) FAIL / NO_UPDATE → set phase and return.
     * 4) UPDATE: skip if !force and latestVersion == ota_applied; else
     *    fw_ota_begin → stream presigned URL → save_applied → reboot.
     */
    char stripped[24];
    const esp_app_desc_t *app = esp_app_get_description();
    fw_ota_strip_version(app && app->version[0] ? app->version : "", stripped, sizeof(stripped));

    xSemaphoreTake(s_mu, portMAX_DELAY);
    snprintf(s_st.current_version, sizeof(s_st.current_version), "%s", stripped);
    xSemaphoreGive(s_mu);

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, "no_mem");
        xSemaphoreGive(s_mu);
        return ESP_ERR_NO_MEM;
    }
    cJSON *input = cJSON_AddObjectToObject(body, "input");
    if (!input) {
        cJSON_Delete(body);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, "no_mem");
        xSemaphoreGive(s_mu);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(input, "deviceId", cfg.device_id);
    cJSON_AddStringToObject(input, "manufacturer", CONFIG_FW_OTA_LTE_MANUFACTURER);
    cJSON_AddStringToObject(input, "deviceType", CONFIG_FW_OTA_LTE_DEVICE_TYPE);
    cJSON_AddStringToObject(input, "currentVersion", stripped[0] ? stripped : "0.0.0");
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!payload) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, "no_mem");
        xSemaphoreGive(s_mu);
        return ESP_ERR_NO_MEM;
    }

    net_lte_http_req_headers_t hdr = {
        .authorization = CONFIG_FW_OTA_LTE_AUTH_HEADER,
        .system_user_id = CONFIG_FW_OTA_LTE_SYSTEM_USER_ID,
    };

    char *resp = malloc(CHECK_RESP_BUF_LEN);
    if (!resp) {
        free(payload);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, "no_mem");
        xSemaphoreGive(s_mu);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "firmware-check POST: %s (currentVersion=%s)", cfg.manifest_url,
             stripped[0] ? stripped : "0.0.0");

    net_lte_http_result_t hr;
    memset(&hr, 0, sizeof(hr));
    size_t got = 0;
    esp_err_t err = net_lte_http_post_recv(cfg.manifest_url, payload, &hdr, resp, CHECK_RESP_BUF_LEN,
                                          &got, &hr);
    free(payload);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.http_status = hr.http_status;
    xSemaphoreGive(s_mu);

    if (err != ESP_OK) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, hr.error[0] ? hr.error : "check_post_fail");
        xSemaphoreGive(s_mu);
        free(resp);
        return err;
    }

    fw_ota_check_result_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    if (fw_ota_parse_check_json(resp, stripped, &parsed) != 0) {
        free(resp);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, "parse_error");
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

    if (parsed.kind == FW_OTA_CHECK_FAIL) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, parsed.error[0] ? parsed.error : "check_fail");
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (parsed.kind == FW_OTA_CHECK_NO_UPDATE) {
        ESP_LOGI(TAG, "no_update (updateAvailable=%d latest='%s')", (int)parsed.update_available,
                 parsed.latest_version);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_NO_UPDATE, NULL);
        xSemaphoreGive(s_mu);
        return ESP_OK;
    }

    /* FW_OTA_CHECK_UPDATE */
    if (!cfg.force && s_applied[0] && strcmp(s_applied, parsed.latest_version) == 0) {
        ESP_LOGI(TAG, "latest %s already applied — no_update", parsed.latest_version);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_NO_UPDATE, NULL);
        xSemaphoreGive(s_mu);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "update -> %s size=%u", parsed.latest_version, (unsigned)parsed.size);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    set_phase(FW_OTA_LTE_DOWNLOADING, NULL);
    xSemaphoreGive(s_mu);

    /*
     * Stream the binary into fw_ota_write() (no full-.bin RAM buffer).
     * Retry a few times on stream/read fail so trucks recover without SoftAP.
     * GPS background AT is paused by lte_ota_task for this whole worker.
     */
    err = ESP_FAIL;
    for (int attempt = 1; attempt <= FW_OTA_LTE_STREAM_ATTEMPTS; ++attempt) {
        err = fw_ota_begin(parsed.size, parsed.sha256);
        if (err != ESP_OK) {
            fw_ota_status_t ost;
            fw_ota_get_status(&ost);
            xSemaphoreTake(s_mu, portMAX_DELAY);
            set_phase(FW_OTA_LTE_FAILED, ost.error[0] ? ost.error : "fw_ota_begin");
            xSemaphoreGive(s_mu);
            return err;
        }

        size_t clen = 0;
        memset(&hr, 0, sizeof(hr));
        ESP_LOGI(TAG, "bin stream attempt %d/%d", attempt, FW_OTA_LTE_STREAM_ATTEMPTS);
        err = net_lte_http_get_stream(parsed.presigned_url, stream_write_cb, NULL, &clen, &hr);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_st.http_status = hr.http_status;
        xSemaphoreGive(s_mu);

        if (err == ESP_OK) {
            break;
        }

        ESP_LOGW(TAG, "bin stream fail attempt %d/%d: %s", attempt, FW_OTA_LTE_STREAM_ATTEMPTS,
                 hr.error[0] ? hr.error : esp_err_to_name(err));
        fw_ota_abort();
        if (attempt < FW_OTA_LTE_STREAM_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(FW_OTA_LTE_STREAM_RETRY_MS));
        }
    }

    char pending_ver[40];
    snprintf(pending_ver, sizeof(pending_ver), "%s", parsed.latest_version);

    if (err != ESP_OK) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, hr.error[0] ? hr.error : "bin_download_fail");
        xSemaphoreGive(s_mu);
        return err;
    }

    /* Persist as pending only — promote to ota_applied after boot verify (B1). */
    (void)save_pending_version(pending_ver);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    set_phase(FW_OTA_LTE_REBOOTING, NULL);
    xSemaphoreGive(s_mu);

    err = fw_ota_end_and_reboot();
    /* only on failure — drop pending so a rollback can redownload */
    (void)clear_pending_version();
    fw_ota_abort();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    set_phase(FW_OTA_LTE_FAILED, "end_reboot_fail");
    xSemaphoreGive(s_mu);
    return err;
}

/** @brief Worker: suspend BG AT, fw_ota_lte_run, resume, clear s_task. */
static void lte_ota_task(void *arg)
{
    (void)arg;
    /* Also cover firmware-check POST: no GPS AT while OTA worker runs. */
    net_lte_suspend_bg_at(true);
    fw_ota_lte_run();
    net_lte_suspend_bg_at(false);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t fw_ota_lte_start_background(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fw_ota_is_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(lte_ota_task, "fw_ota_lte", 8192, NULL, 5, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool fw_ota_lte_is_busy(void)
{
    fw_ota_lte_status_t st;
    if (fw_ota_lte_get_status(&st) != ESP_OK) {
        return false;
    }
    return st.phase == FW_OTA_LTE_CHECKING || st.phase == FW_OTA_LTE_DOWNLOADING ||
           st.phase == FW_OTA_LTE_REBOOTING || s_task != NULL;
}

esp_err_t fw_ota_lte_commit_pending_applied(void)
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

#if CONFIG_FW_OTA_LTE_AUTO_CHECK

/** @brief Poll net_lte_refresh until registered or timeout. */
static bool wait_lte_registered(int max_sec)
{
    for (int i = 0; i < max_sec; i++) {
        (void)net_lte_refresh();
        net_lte_status_t st;
        if (net_lte_get_status(&st) == ESP_OK && st.enabled && st.sim_ready && st.registered) {
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
    fw_ota_lte_config_t cfg;
    if (fw_ota_lte_get_config(&cfg) != ESP_OK) {
        return;
    }
    bool dirty = false;
    if (cfg.manifest_url[0] == '\0') {
        apply_default_url(&cfg);
        dirty = cfg.manifest_url[0] != '\0';
    }
    /* Auto path never force-reflashes */
    if (cfg.force) {
        cfg.force = false;
        dirty = true;
    }
    if (dirty) {
        (void)fw_ota_lte_set_config(&cfg);
    }
}

/** @brief One auto cycle: start_background and wait until not busy. */
static void auto_check_once(void)
{
    ensure_auto_config();
    fw_ota_lte_config_t cfg;
    if (fw_ota_lte_get_config(&cfg) != ESP_OK || cfg.manifest_url[0] == '\0') {
        ESP_LOGW(TAG, "auto: no check URL — skip");
        return;
    }
    if (cfg.device_id[0] == '\0') {
        ESP_LOGW(TAG, "auto: device_id not provisioned — skip");
        return;
    }
    if (fw_ota_lte_is_busy() || fw_ota_is_busy()) {
        ESP_LOGW(TAG, "auto: OTA busy — skip this cycle");
        return;
    }
    ESP_LOGI(TAG, "auto: starting OTA check url='%s'", cfg.manifest_url);
    esp_err_t err = fw_ota_lte_start_background();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "auto: start failed: %s", esp_err_to_name(err));
        return;
    }
    /* Wait until worker finishes (reboot never returns) */
    while (fw_ota_lte_is_busy()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/** @brief Infinite auto-check loop with interval sleep. */
static void lte_ota_auto_task(void *arg)
{
    (void)arg;
    const int wait_sec = CONFIG_FW_OTA_LTE_AUTO_WAIT_SEC;
    const int interval_h = CONFIG_FW_OTA_LTE_AUTO_INTERVAL_H;
    ESP_LOGI(TAG, "auto: task started (wait=%ds interval=%dh)", wait_sec, interval_h);

    for (;;) {
        wait_lte_registered(wait_sec);
        auto_check_once();
        ESP_LOGI(TAG, "auto: next check in %d hour(s)", interval_h);
        vTaskDelay(pdMS_TO_TICKS((uint32_t)interval_h * 3600U * 1000U));
    }
}

esp_err_t fw_ota_lte_start_auto(void)
{
    if (s_auto_task != NULL) {
        return ESP_OK;
    }
    if (xTaskCreate(lte_ota_auto_task, "fw_ota_auto", 4096, NULL, 4, &s_auto_task) != pdPASS) {
        s_auto_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else /* !CONFIG_FW_OTA_LTE_AUTO_CHECK */

esp_err_t fw_ota_lte_start_auto(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
