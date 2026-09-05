#include "uplink.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host_registry.h"
#include "lte.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "obd_poller.h"
#include "ota_cloud.h"
#include "sdkconfig.h"
#include "store_sd.h"
#include "uplink_batch.h"
#include "uplink_envelope.h"
#include "uplink_host.h"
#include "uplink_obd.h"
#include "uplink_queue.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "uplink";

#ifndef CONFIG_UPLINK_URL
#error "CONFIG_UPLINK_URL missing — set it in firmware_v2/master/sdkconfig.defaults"
#endif

#ifndef CONFIG_UPLINK_TICK_SEC
#define CONFIG_UPLINK_TICK_SEC 30
#endif

#define NVS_NS "uplink"
#define NVS_KEY_NID "node_id"
/* Legacy lab override — erased on boot so CONFIG_UPLINK_URL from the bin wins. */
#define NVS_KEY_URL_LEGACY "url"

/* One tick's envelopes + POST body — kept off task stacks (caller holds s_mu). */
static uplink_batch_t s_batch;
static char s_post_body[UPLINK_BATCH_MAX_EVENTS * UPLINK_BATCH_ENV_MAX + 32];
/* Vehicle wrap / drain bodies can be larger than one tick. */
static char s_wrap_body[16384];

#ifndef CONFIG_UPLINK_VEHICLE_WRAP
#define CONFIG_UPLINK_VEHICLE_WRAP 1
#endif

static SemaphoreHandle_t s_mu;
static char s_node_id[40];
static char s_url[256];
static uplink_status_t s_st;
static bool s_have_gps_last;
static double s_last_lat, s_last_lng;
static uint64_t s_last_gps_send_ms;
static TaskHandle_t s_task;
static TaskHandle_t s_drain_task;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static void enqueue_failed_envelope(const char *envelope_json)
{
    if (!store_sd_is_mounted() || envelope_json == NULL || envelope_json[0] == '\0') {
        return;
    }
    char line[900];
    int n = uplink_queue_line_from_envelope(envelope_json, now_ms(), line, sizeof(line));
    if (n < 0) {
        return;
    }
    if (store_sd_enqueue_line(line, (size_t)n) == ESP_OK) {
        s_st.queue_enqueued++;
        ESP_LOGI(TAG, "queued failed POST (%d bytes)", n);
    }
}

static esp_err_t post_json_body(const char *schema_tag, const char *body, size_t body_len)
{
#if CONFIG_UPLINK_VEHICLE_WRAP
    /* Trafyn requires top-level "Vehicle"; wrap the envelope array. */
    int wn = snprintf(s_wrap_body, sizeof(s_wrap_body), "{\"Vehicle\":%s}", body);
    if (wn < 0 || (size_t)wn >= sizeof(s_wrap_body)) {
        return ESP_ERR_NO_MEM;
    }
    body = s_wrap_body;
    body_len = (size_t)wn;
#endif
    ESP_LOGI(TAG, "POST %s %s (%u bytes)", schema_tag, s_url, (unsigned)body_len);
    lte_http_result_t hr;
    memset(&hr, 0, sizeof(hr));
    esp_err_t err = lte_http_post(s_url, body, &hr);
    s_st.last_http_status = hr.http_status;
    if (err == ESP_OK && hr.http_status >= 200 && hr.http_status < 300) {
        s_st.send_ok++;
        s_st.last_error[0] = '\0';
        s_st.last_send_ms = now_ms();
        ESP_LOGI(TAG, "%s ok http=%d", schema_tag, hr.http_status);
        return ESP_OK;
    }
    s_st.send_fail++;
    snprintf(s_st.last_error, sizeof(s_st.last_error), "%s",
             hr.error[0] ? hr.error : esp_err_to_name(err));
    ESP_LOGW(TAG, "%s fail http=%d err=%s", schema_tag, hr.http_status, s_st.last_error);
    return err != ESP_OK ? err : ESP_FAIL;
}

/** @brief Load node_id from NVS; drop any legacy URL override so bin wins. */
static void load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_node_id);
    (void)nvs_get_str(h, NVS_KEY_NID, s_node_id, &len);
    if (nvs_erase_key(h, NVS_KEY_URL_LEGACY) == ESP_OK) {
        (void)nvs_commit(h);
        ESP_LOGI(TAG, "cleared legacy NVS uplink URL (using bin CONFIG_UPLINK_URL)");
    }
    nvs_close(h);
}

static esp_err_t save_node_id(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_NID, s_node_id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

#ifndef CONFIG_STORE_SD_BATCH_MAX_EVENTS
#define CONFIG_STORE_SD_BATCH_MAX_EVENTS 10
#endif

static void drain_once(void)
{
    if (!store_sd_is_mounted()) {
        return;
    }
    /* ~27 KiB — must stay off the drain task stack (caller holds s_mu). */
    static char peek[12288];
    static char post_body[14336];
    size_t n_lines = 0;
    size_t byte_span = 0;
    size_t max_lines = CONFIG_STORE_SD_BATCH_MAX_EVENTS;
    if (store_sd_peek_lines(peek, sizeof(peek), max_lines, &n_lines, &byte_span) != ESP_OK ||
        n_lines == 0) {
        return;
    }

    int bn = uplink_queue_build_batch(peek, n_lines, post_body, sizeof(post_body));
    if (bn < 0) {
        (void)store_sd_ack_bytes(byte_span, n_lines);
        return;
    }

    if (post_json_body("drain", post_body, (size_t)bn) == ESP_OK) {
        (void)store_sd_ack_bytes(byte_span, n_lines);
        s_st.queue_drained += (uint32_t)n_lines;
    }
}

static void drain_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(60000));
    for (;;) {
        if (s_st.enabled && xSemaphoreTake(s_mu, pdMS_TO_TICKS(2000)) == pdTRUE) {
            drain_once();
            xSemaphoreGive(s_mu);
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

static void copy_pid(uplink_pid_view_t *dst, const obd_pid_view_t *src)
{
    memset(dst, 0, sizeof(*dst));
    if (src == NULL) {
        return;
    }
    dst->valid = src->valid;
    dst->ok = src->ok;
    dst->value = src->value;
    dst->age_ms = src->age_ms;
    snprintf(dst->raw, sizeof(dst->raw), "%s", src->raw);
}

/** Append one OBD 1087 envelope when ≥1 fresh PID. */
static void collect_obd(uplink_batch_t *batch)
{
    ota_cloud_config_t ocfg;
    memset(&ocfg, 0, sizeof(ocfg));
    if (ota_cloud_get_config(&ocfg) != ESP_OK || ocfg.device_id[0] == '\0') {
        s_st.skip_no_id++;
        return;
    }
    if (s_node_id[0] == '\0') {
        s_st.skip_no_id++;
        return;
    }

    obd_poller_snapshot_t ps;
    memset(&ps, 0, sizeof(ps));
    if (obd_poller_get_snapshot(&ps) != ESP_OK || !obd_poller_has_fresh_pid(&ps)) {
        s_st.skip_no_obd++;
        return;
    }

    uplink_obd_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snprintf(snap.obd_profile, sizeof(snap.obd_profile), "%s", "mode01_core");
    snprintf(snap.obd_protocol, sizeof(snap.obd_protocol), "%s", ps.protocol);
    snap.uptime_seconds = ps.uptime_seconds;
    snap.poller_status = ps.enabled ? "on" : "paused";
    snap.cmds_ok = ps.cmds_ok;
    snap.cmds_fail = ps.cmds_fail;
    copy_pid(&snap.rpm, &ps.rpm);
    copy_pid(&snap.speed, &ps.speed);
    copy_pid(&snap.coolant, &ps.coolant);
    copy_pid(&snap.throttle, &ps.throttle);

    char payload[640];
    char env[UPLINK_BATCH_ENV_MAX];
    if (uplink_build_obd_payload(&snap, payload, sizeof(payload)) < 0) {
        return;
    }
    uint64_t ts = lte_time_now_ms();
    if (ts == 0) {
        ts = now_ms();
    }
    if (uplink_build_envelope(ocfg.device_id, s_node_id, UPLINK_SCHEMA_OBD, ts, payload, env,
                              sizeof(env)) < 0) {
        return;
    }
    (void)uplink_batch_add(batch, env);
}

/** Append one 1088 envelope per Zigbee host with a valid reading. */
static void collect_hosts(uplink_batch_t *batch)
{
    static fleet_registry_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    if (!host_registry_snapshot(&snap) || snap.host_count == 0) {
        s_st.skip_no_host++;
        return;
    }

    uint64_t ts = lte_time_now_ms();
    if (ts == 0) {
        ts = now_ms();
    }

    bool any = false;
    for (uint8_t i = 0; i < snap.host_count && i < FLEET_REGISTRY_MAX_HOSTS; i++) {
        const fleet_registry_host_t *h = &snap.hosts[i];
        if (h->device_id[0] == '\0' || h->schema_id[0] == '\0' || h->node_id[0] == '\0') {
            continue;
        }

        uplink_host_report_t report;
        memset(&report, 0, sizeof(report));
        snprintf(report.device_id, sizeof(report.device_id), "%s", h->device_id);
        snprintf(report.node_id, sizeof(report.node_id), "%s", h->node_id);
        snprintf(report.schema_id, sizeof(report.schema_id), "%s", h->schema_id);
        snprintf(report.host_type, sizeof(report.host_type), "%s", h->host_type);
        report.reading_count = h->reading_count;
        if (report.reading_count > UPLINK_HOST_MAX_READINGS) {
            report.reading_count = UPLINK_HOST_MAX_READINGS;
        }
        for (uint8_t r = 0; r < report.reading_count; r++) {
            snprintf(report.readings[r].key, sizeof(report.readings[r].key), "%s",
                     h->readings[r].key);
            snprintf(report.readings[r].unit, sizeof(report.readings[r].unit), "%s",
                     h->readings[r].unit);
            report.readings[r].value = h->readings[r].value;
            report.readings[r].valid = h->readings[r].valid;
        }
        if (!uplink_host_has_valid_reading(&report)) {
            continue;
        }

        char payload[512];
        char env[UPLINK_BATCH_ENV_MAX];
        if (uplink_build_host_report_payload(&report, payload, sizeof(payload)) < 0) {
            continue;
        }
        const char *schema =
            report.schema_id[0] ? report.schema_id : UPLINK_SCHEMA_HOST;
        if (uplink_build_envelope(report.device_id, report.node_id, schema, ts, payload, env,
                                  sizeof(env)) < 0) {
            continue;
        }
        if (uplink_batch_add(batch, env) == 0) {
            any = true;
        }
    }
    if (!any) {
        s_st.skip_no_host++;
    }
}

/** Append one GPS 1089 envelope when fix + gate allow. */
static void collect_gps(uplink_batch_t *batch, bool force)
{
    ota_cloud_config_t ocfg;
    memset(&ocfg, 0, sizeof(ocfg));
    if (ota_cloud_get_config(&ocfg) != ESP_OK || ocfg.device_id[0] == '\0') {
        s_st.skip_no_id++;
        return;
    }
    if (s_node_id[0] == '\0') {
        s_st.skip_no_id++;
        return;
    }

    lte_gps_t gps;
    memset(&gps, 0, sizeof(gps));
    if (lte_gps_get(&gps) != ESP_OK || !gps.gps_ok) {
        s_st.skip_no_gps++;
        return;
    }

    uint64_t ts = lte_time_now_ms();
    if (ts == 0) {
        ts = now_ms();
    }
    uint64_t gate_now = now_ms();
    if (!force &&
        !uplink_gps_worth_sending(s_have_gps_last, s_last_lat, s_last_lng, s_last_gps_send_ms,
                                  gps.lat, gps.lng, gate_now)) {
        s_st.skip_gate++;
        return;
    }

    char payload[128];
    char gps_did[40];
    char gps_nid[40];
    char env[UPLINK_BATCH_ENV_MAX];
    if (uplink_build_gps_payload(true, gps.lat, gps.lng, payload, sizeof(payload)) < 0) {
        return;
    }
    uplink_virtual_gps_ids(ocfg.device_id, gps_did, sizeof(gps_did), gps_nid, sizeof(gps_nid));
    if (uplink_build_envelope(gps_did, gps_nid, UPLINK_SCHEMA_GPS, ts, payload, env,
                              sizeof(env)) < 0) {
        return;
    }
    if (uplink_batch_add(batch, env) == 0) {
        s_have_gps_last = true;
        s_last_lat = gps.lat;
        s_last_lng = gps.lng;
        s_last_gps_send_ms = gate_now;
    }
}

/**
 * Core tick: dynamically collect OBD / Zigbee hosts / GPS envelopes, then one array POST.
 * On fail, each bare envelope is queued to SD (store-on-fail).
 */
static esp_err_t produce_tick(bool force_gps)
{
    uplink_batch_clear(&s_batch);
    collect_obd(&s_batch);
    collect_hosts(&s_batch);
    collect_gps(&s_batch, force_gps);

    if (s_batch.count == 0) {
        snprintf(s_st.last_error, sizeof(s_st.last_error), "no events this tick");
        return ESP_ERR_NOT_FOUND;
    }

    int bn = uplink_batch_build_array(&s_batch, s_post_body, sizeof(s_post_body));
    if (bn < 0) {
        return ESP_ERR_NO_MEM;
    }

    char tag[32];
    snprintf(tag, sizeof(tag), "batch/%u", (unsigned)s_batch.count);
    esp_err_t err = post_json_body(tag, s_post_body, (size_t)bn);
    if (err != ESP_OK) {
        for (size_t i = 0; i < s_batch.count; i++) {
            enqueue_failed_envelope(s_batch.events[i]);
        }
    }
    return err;
}

static void uplink_task(void *arg)
{
    (void)arg;
    const int tick_sec = CONFIG_UPLINK_TICK_SEC > 0 ? CONFIG_UPLINK_TICK_SEC : 30;
    vTaskDelay(pdMS_TO_TICKS(45000));
    for (;;) {
        if (s_st.enabled) {
            if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(5000)) == pdTRUE) {
                (void)produce_tick(false);
                xSemaphoreGive(s_mu);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(tick_sec * 1000));
    }
}

esp_err_t uplink_init(void)
{
    if (s_mu == NULL) {
        s_mu = xSemaphoreCreateMutex();
        if (s_mu == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(&s_st, 0, sizeof(s_st));
    s_st.enabled = true;
    snprintf(s_url, sizeof(s_url), "%s", CONFIG_UPLINK_URL);
    s_node_id[0] = '\0';
    load_nvs();
    ESP_LOGI(TAG, "init url='%s' node_id='%s'", s_url, s_node_id[0] ? s_node_id : "(empty)");
    return ESP_OK;
}

esp_err_t uplink_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (xTaskCreate(uplink_task, "uplink", 8192, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (s_drain_task == NULL) {
        if (xTaskCreate(drain_task, "uplink_drain", 8192, NULL, 3, &s_drain_task) != pdPASS) {
            s_drain_task = NULL;
            ESP_LOGW(TAG, "drain task not started");
        }
    }
    return ESP_OK;
}

esp_err_t uplink_get_status(uplink_status_t *out)
{
    if (out == NULL || s_mu == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_st;
    snprintf(out->node_id, sizeof(out->node_id), "%s", s_node_id);
    snprintf(out->url, sizeof(out->url), "%s", s_url);
    if (store_sd_is_mounted()) {
        store_sd_status_t sd;
        memset(&sd, 0, sizeof(sd));
        if (store_sd_get_status(&sd) == ESP_OK) {
            out->queue_depth = sd.depth;
        }
    }
    xSemaphoreGive(s_mu);
    return ESP_OK;
}

esp_err_t uplink_set_node_id(const char *node_id)
{
    if (node_id == NULL || s_mu == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    snprintf(s_node_id, sizeof(s_node_id), "%s", node_id);
    esp_err_t err = save_node_id();
    xSemaphoreGive(s_mu);
    return err;
}

esp_err_t uplink_get_node_id(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0 || s_mu == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    snprintf(out, out_len, "%s", s_node_id);
    xSemaphoreGive(s_mu);
    return ESP_OK;
}

esp_err_t uplink_set_post_url(const char *url)
{
    (void)url;
    /* Fleet-wide URL is baked into the bin (CONFIG_UPLINK_URL). Change via OTA. */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t uplink_once(void)
{
    if (s_mu == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(120000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = produce_tick(true);
    xSemaphoreGive(s_mu);
    return err;
}

esp_err_t uplink_lab_post(void)
{
    if (s_mu == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(120000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ota_cloud_config_t ocfg;
    memset(&ocfg, 0, sizeof(ocfg));
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (ota_cloud_get_config(&ocfg) != ESP_OK || ocfg.device_id[0] == '\0') {
        snprintf(s_st.last_error, sizeof(s_st.last_error), "no device_id");
        goto out;
    }
    if (s_node_id[0] == '\0') {
        snprintf(s_st.last_error, sizeof(s_st.last_error), "no node_id");
        goto out;
    }

    char payload[128];
    char gps_did[48];
    char gps_nid[56];
    char env[UPLINK_BATCH_ENV_MAX];
    if (uplink_build_gps_payload(true, 12.9716, 77.5946, payload, sizeof(payload)) < 0) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    uplink_virtual_gps_ids(ocfg.device_id, gps_did, sizeof(gps_did), gps_nid, sizeof(gps_nid));
    uint64_t ts = lte_time_now_ms();
    if (ts == 0) {
        ts = now_ms();
    }
    if (uplink_build_envelope(gps_did, gps_nid, UPLINK_SCHEMA_GPS, ts, payload, env,
                              sizeof(env)) < 0) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    uplink_batch_clear(&s_batch);
    if (uplink_batch_add(&s_batch, env) != 0) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    int bn = uplink_batch_build_array(&s_batch, s_post_body, sizeof(s_post_body));
    if (bn < 0) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    err = post_json_body("lab/1089", s_post_body, (size_t)bn);
    if (err != ESP_OK) {
        enqueue_failed_envelope(env);
    }

out:
    xSemaphoreGive(s_mu);
    return err;
}
