#include "fw_ota_lte.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fw_ota.h"
#include "net_lte.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "cJSON.h"

#include <stdio.h>
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
 * - ota_manif: base manifest URL (e.g. https://.../firmware/manifest)
 *              The device appends: ?device_id=<id>&channel=<channel>
 * - ota_chan:  manifest channel string (currently "stable")
 * - ota_force: if true, reflash even when version matches
 * - uplink_did: uplink/device_id; kept in sync with OTA "device_id"
 * - ota_applied: last successfully applied manifest "version" (prevents lab label-bumps
 *                 from causing infinite redownload loops)
 */
#define NVS_KEY_URL "ota_manif"
#define NVS_KEY_CHAN "ota_chan"
#define NVS_KEY_FORCE "ota_force"
#define NVS_KEY_DID "uplink_did"
#define NVS_KEY_APPLIED "ota_applied"

/* Manifest JSON is small (version/url/sha256/size). Keep this buffer tight. */
#define MANIFEST_BUF_LEN 1536

#ifndef CONFIG_FW_OTA_LTE_DEFAULT_MANIFEST_URL
#define CONFIG_FW_OTA_LTE_DEFAULT_MANIFEST_URL ""
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

static void set_phase(fw_ota_lte_phase_t p, const char *err)
{
    s_st.phase = p;
    if (err) {
        snprintf(s_st.error, sizeof(s_st.error), "%s", err);
    } else if (p != FW_OTA_LTE_FAILED) {
        s_st.error[0] = '\0';
    }
}

static void apply_default_url(fw_ota_lte_config_t *c)
{
    if (c->manifest_url[0] != '\0') {
        return;
    }
    const char *def = CONFIG_FW_OTA_LTE_DEFAULT_MANIFEST_URL;
    if (def && def[0]) {
        snprintf(c->manifest_url, sizeof(c->manifest_url), "%s", def);
    }
}

static void defaults(fw_ota_lte_config_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->channel, sizeof(c->channel), "%s", "stable");
    snprintf(c->device_id, sizeof(c->device_id), "%s", "fleet-demo-001");
    c->force = false;
    c->manifest_url[0] = '\0';
    apply_default_url(c);
}

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
    apply_default_url(&s_cfg);
    if (s_cfg.channel[0] == '\0') {
        snprintf(s_cfg.channel, sizeof(s_cfg.channel), "%s", "stable");
    }
    if (s_cfg.device_id[0] == '\0') {
        snprintf(s_cfg.device_id, sizeof(s_cfg.device_id), "%s", "fleet-demo-001");
    }
    return ESP_OK;
}

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

static void build_manifest_url(const fw_ota_lte_config_t *cfg, char *out, size_t out_len)
{
    /* If URL already has '?', append with & else ? */
    const char *sep = strchr(cfg->manifest_url, '?') ? "&" : "?";
    snprintf(out, out_len, "%s%sdevice_id=%s&channel=%s", cfg->manifest_url, sep,
             cfg->device_id[0] ? cfg->device_id : "fleet-demo-001",
             cfg->channel[0] ? cfg->channel : "stable");
}

esp_err_t fw_ota_lte_run(void)
{
    fw_ota_lte_config_t cfg;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    cfg = s_cfg;
    s_st.bytes_downloaded = 0;
    s_st.http_status = 0;
    s_st.manifest_version[0] = '\0';
    set_phase(FW_OTA_LTE_CHECKING, NULL);
    xSemaphoreGive(s_mu);

    if (cfg.manifest_url[0] == '\0') {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, "manifest_url empty");
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * OTA algorithm (LTE path):
     *
     * 1) Build full manifest request URL by appending the device identity.
     * 2) GET the manifest JSON over LTE (small response).
     * 3) Parse required fields: version, url (bin), sha256, size.
     * 4) Version decision:
     *      - If force=false and manifest.version == running app version => no_update
     *      - If force=false and manifest.version == ota_applied (last successful) => no_update
     *      - Otherwise: start OTA download.
     * 5) Download bin over LTE using QHTTPGET streaming and write chunks into fw_ota.
     *    fw_ota_begin() chooses the inactive ota_X slot using esp_ota_get_next_update_partition().
     * 6) Verify sha256 + size inside fw_ota_end_and_reboot().
     * 7) Persist ota_applied and reboot (bootloader+otadata selects the new ota_X).
     */
    char url[256];
    build_manifest_url(&cfg, url, sizeof(url));
    ESP_LOGI(TAG, "fetching manifest: %s", url);

    char *manif = malloc(MANIFEST_BUF_LEN);
    if (!manif) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, "no_mem");
        xSemaphoreGive(s_mu);
        return ESP_ERR_NO_MEM;
    }

    net_lte_http_result_t hr;
    size_t got = 0;
    esp_err_t err = net_lte_http_get(url, manif, MANIFEST_BUF_LEN, &got, &hr);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.http_status = hr.http_status;
    xSemaphoreGive(s_mu);

    if (err != ESP_OK) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, hr.error[0] ? hr.error : "manifest_get_fail");
        xSemaphoreGive(s_mu);
        free(manif);
        return err;
    }

    cJSON *root = cJSON_Parse(manif);
    if (!root) {
        /* Skip leading modem/noise; find first JSON object. */
        const char *brace = strchr(manif, '{');
        if (brace) {
            root = cJSON_Parse(brace);
        }
    }
    if (!root) {
        char preview[64];
        snprintf(preview, sizeof(preview), "%s", manif[0] ? manif : "(empty)");
        for (char *p = preview; *p; ++p) {
            if (*p < 32 || *p > 126) {
                *p = '.';
            }
        }
        ESP_LOGE(TAG, "manifest JSON parse fail (len=%u): %s", (unsigned)got, preview);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        snprintf(s_st.error, sizeof(s_st.error), "manifest_json:%.40s", preview);
        s_st.phase = FW_OTA_LTE_FAILED;
        xSemaphoreGive(s_mu);
        free(manif);
        return ESP_ERR_INVALID_RESPONSE;
    }
    free(manif);

    const cJSON *jver = cJSON_GetObjectItem(root, "version");
    const cJSON *jurl = cJSON_GetObjectItem(root, "url");
    const cJSON *jsha = cJSON_GetObjectItem(root, "sha256");
    const cJSON *jsize = cJSON_GetObjectItem(root, "size");
    if (!cJSON_IsString(jver) || !cJSON_IsString(jurl) || !cJSON_IsString(jsha) ||
        !cJSON_IsNumber(jsize)) {
        cJSON_Delete(root);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, "manifest_fields");
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *version = jver->valuestring;
    const char *bin_url = jurl->valuestring;
    const char *sha = jsha->valuestring;
    size_t size = (size_t)jsize->valuedouble;

    xSemaphoreTake(s_mu, portMAX_DELAY);
    snprintf(s_st.manifest_version, sizeof(s_st.manifest_version), "%s", version);
    xSemaphoreGive(s_mu);

    const esp_app_desc_t *app = esp_app_get_description();
    const char *running = app ? app->version : "";
    /* Skip if same as running build OR same as last successfully applied manifest. */
    if (!cfg.force) {
        if (running[0] && strcmp(running, version) == 0) {
            ESP_LOGI(TAG, "already on version %s — no_update", version);
            cJSON_Delete(root);
            xSemaphoreTake(s_mu, portMAX_DELAY);
            set_phase(FW_OTA_LTE_NO_UPDATE, NULL);
            xSemaphoreGive(s_mu);
            return ESP_OK;
        }
        if (s_applied[0] && strcmp(s_applied, version) == 0) {
            ESP_LOGI(TAG, "manifest %s already applied — no_update", version);
            cJSON_Delete(root);
            xSemaphoreTake(s_mu, portMAX_DELAY);
            set_phase(FW_OTA_LTE_NO_UPDATE, NULL);
            xSemaphoreGive(s_mu);
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "update %s -> %s size=%u", running, version, (unsigned)size);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    set_phase(FW_OTA_LTE_DOWNLOADING, NULL);
    xSemaphoreGive(s_mu);

    err = fw_ota_begin(size, sha);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        fw_ota_status_t ost;
        fw_ota_get_status(&ost);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, ost.error[0] ? ost.error : "fw_ota_begin");
        xSemaphoreGive(s_mu);
        return err;
    }

    size_t clen = 0;
    /*
     * Stream the binary and immediately forward each chunk into fw_ota_write().
     * This avoids buffering the full .bin in RAM.
     */
    err = net_lte_http_get_stream(bin_url, stream_write_cb, NULL, &clen, &hr);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.http_status = hr.http_status;
    xSemaphoreGive(s_mu);

    /* Keep strings from JSON until begin used them — sha/url copied already into fw_ota. */
    char applied_ver[40];
    snprintf(applied_ver, sizeof(applied_ver), "%s", version);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        fw_ota_abort();
        xSemaphoreTake(s_mu, portMAX_DELAY);
        set_phase(FW_OTA_LTE_FAILED, hr.error[0] ? hr.error : "bin_download_fail");
        xSemaphoreGive(s_mu);
        return err;
    }

    /* Remember manifest version so lab label bumps don't re-download forever. */
    (void)save_applied_version(applied_ver);

    xSemaphoreTake(s_mu, portMAX_DELAY);
    set_phase(FW_OTA_LTE_REBOOTING, NULL);
    xSemaphoreGive(s_mu);

    err = fw_ota_end_and_reboot();
    /* only on failure */
    fw_ota_abort();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    set_phase(FW_OTA_LTE_FAILED, "end_reboot_fail");
    xSemaphoreGive(s_mu);
    return err;
}

static void lte_ota_task(void *arg)
{
    (void)arg;
    fw_ota_lte_run();
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

#if CONFIG_FW_OTA_LTE_AUTO_CHECK

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

static void auto_check_once(void)
{
    ensure_auto_config();
    fw_ota_lte_config_t cfg;
    if (fw_ota_lte_get_config(&cfg) != ESP_OK || cfg.manifest_url[0] == '\0') {
        ESP_LOGW(TAG, "auto: no manifest URL — skip");
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
