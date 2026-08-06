#include "telemetry_uplink.h"
#include "uplink_payload.h"

#include "can_obd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fw_ota_lte.h"
#include "net_lte.h"
#include "nvs.h"
#include "obd_poller.h"
#include "profile_store.h"
#include "sdkconfig.h"
#include "store_sd.h"
#include "sys_runtime.h"
#include "telemetry_bus.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "uplink";

#define NVS_NS "elm"
#define NVS_KEY_EN "uplink_en"
#define NVS_KEY_IV "uplink_iv"
#define NVS_KEY_DID "uplink_did"
#define NVS_KEY_NID "uplink_nid"

#define UPLINK_TASK_STACK 8192
#define UPLINK_DRAIN_STACK 8192
#define UPLINK_TASK_PRIO 4
#define UPLINK_CACHE_TASK_STACK 3072
#define PAYLOAD_BUF_LEN 1800
#define EVENT_BUF_LEN 1900
#define BATCH_BUF_LEN 13000
#define PEEK_BUF_LEN 12000

#ifndef CONFIG_STORE_SD_BATCH_MAX_EVENTS
#define CONFIG_STORE_SD_BATCH_MAX_EVENTS 10
#endif
#ifndef CONFIG_STORE_SD_BATCH_MAX_BYTES
#define CONFIG_STORE_SD_BATCH_MAX_BYTES 12288
#endif

static telemetry_uplink_config_t s_cfg;
static telemetry_uplink_last_t s_last;
static char s_drain_err[80];
static SemaphoreHandle_t s_mu;
static QueueHandle_t s_q;
static TaskHandle_t s_cache_task;
static TaskHandle_t s_tick_task;
static TaskHandle_t s_drain_task;
static bool s_started;

static telemetry_pid_sample_t s_rpm;
static telemetry_pid_sample_t s_speed;
static telemetry_pid_sample_t s_coolant;
static telemetry_pid_sample_t s_throttle;
static telemetry_pid_sample_t s_voltage;
static bool s_have_rpm, s_have_speed, s_have_coolant, s_have_throttle, s_have_voltage;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static void set_last(bool ok, bool skipped, int http_status, const char *reason,
                     const char *error)
{
    s_last.ok = ok;
    s_last.skipped = skipped;
    s_last.http_status = http_status;
    s_last.ts_ms = now_ms();
    snprintf(s_last.reason, sizeof(s_last.reason), "%s", reason ? reason : "");
    snprintf(s_last.error, sizeof(s_last.error), "%s", error ? error : "");
}

static void set_drain_err(const char *msg)
{
    snprintf(s_drain_err, sizeof(s_drain_err), "%s", msg ? msg : "");
}

static void cfg_defaults(telemetry_uplink_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->enabled = true;
    c->interval_s = 5;
    snprintf(c->device_id, sizeof(c->device_id), "%s", "fleet-demo-001");
    snprintf(c->node_id, sizeof(c->node_id), "%s", "esp32c6-01");
}

static esp_err_t cfg_load(void)
{
    cfg_defaults(&s_cfg);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    uint8_t en = 0;
    if (nvs_get_u8(h, NVS_KEY_EN, &en) == ESP_OK) {
        s_cfg.enabled = en != 0;
    }
    uint16_t iv = 5;
    if (nvs_get_u16(h, NVS_KEY_IV, &iv) == ESP_OK) {
        if (iv < 1) {
            iv = 1;
        }
        if (iv > 300) {
            iv = 300;
        }
        s_cfg.interval_s = iv;
    }
    size_t len = sizeof(s_cfg.device_id);
    nvs_get_str(h, NVS_KEY_DID, s_cfg.device_id, &len);
    len = sizeof(s_cfg.node_id);
    nvs_get_str(h, NVS_KEY_NID, s_cfg.node_id, &len);
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t cfg_save(const telemetry_uplink_config_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_EN, c->enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_u16(h, NVS_KEY_IV, c->interval_s);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_DID, c->device_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_NID, c->node_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void store_sample(const telemetry_pid_sample_t *s)
{
    if (s == NULL || s->name[0] == '\0') {
        return;
    }
    if (strcmp(s->name, "rpm") == 0) {
        s_rpm = *s;
        s_have_rpm = true;
    } else if (strcmp(s->name, "speed") == 0) {
        s_speed = *s;
        s_have_speed = true;
    } else if (strcmp(s->name, "coolant_c") == 0) {
        s_coolant = *s;
        s_have_coolant = true;
    } else if (strcmp(s->name, "throttle_pct") == 0) {
        s_throttle = *s;
        s_have_throttle = true;
    } else if (strcmp(s->name, "voltage") == 0) {
        s_voltage = *s;
        s_have_voltage = true;
    }
}

static void cache_task(void *arg)
{
    (void)arg;
    telemetry_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_q, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.type != TELEMETRY_PID_SAMPLE) {
            continue;
        }
        if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(200)) == pdTRUE) {
            store_sample(&msg.pid_sample);
            xSemaphoreGive(s_mu);
        }
    }
}

static void fill_pid_view(uplink_pid_view_t *v, bool have, const telemetry_pid_sample_t *s,
                          uint64_t now)
{
    memset(v, 0, sizeof(*v));
    if (!have || s == NULL) {
        return;
    }
    v->valid = true;
    v->ok = s->ok;
    v->value = s->value;
    snprintf(v->raw, sizeof(v->raw), "%s", s->raw_hex);
    v->age_ms = (now >= s->ts_ms) ? (uint32_t)(now - s->ts_ms) : 0;
}

static bool any_fresh_ok_locked(uint64_t now)
{
    uplink_pid_view_t v;
    if (s_have_rpm) {
        fill_pid_view(&v, true, &s_rpm, now);
        if (uplink_pid_is_fresh_ok(&v)) {
            return true;
        }
    }
    if (s_have_speed) {
        fill_pid_view(&v, true, &s_speed, now);
        if (uplink_pid_is_fresh_ok(&v)) {
            return true;
        }
    }
    if (s_have_coolant) {
        fill_pid_view(&v, true, &s_coolant, now);
        if (uplink_pid_is_fresh_ok(&v)) {
            return true;
        }
    }
    if (s_have_throttle) {
        fill_pid_view(&v, true, &s_throttle, now);
        if (uplink_pid_is_fresh_ok(&v)) {
            return true;
        }
    }
    if (s_have_voltage) {
        fill_pid_view(&v, true, &s_voltage, now);
        if (uplink_pid_is_fresh_ok(&v)) {
            return true;
        }
    }
    return false;
}

static esp_err_t build_snapshot(uplink_snapshot_t *snap, telemetry_uplink_config_t *cfg_out,
                                bool *have_fresh_out)
{
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *cfg_out = s_cfg;
    uint64_t now = now_ms();
    *have_fresh_out = any_fresh_ok_locked(now);

    memset(snap, 0, sizeof(*snap));
    snprintf(snap->device_id, sizeof(snap->device_id), "%s", s_cfg.device_id);
    snprintf(snap->node_id, sizeof(snap->node_id), "%s", s_cfg.node_id);
    fill_pid_view(&snap->rpm, s_have_rpm, &s_rpm, now);
    fill_pid_view(&snap->speed, s_have_speed, &s_speed, now);
    fill_pid_view(&snap->coolant, s_have_coolant, &s_coolant, now);
    fill_pid_view(&snap->throttle, s_have_throttle, &s_throttle, now);
    fill_pid_view(&snap->voltage, s_have_voltage, &s_voltage, now);
    xSemaphoreGive(s_mu);

    snprintf(snap->ble_peer_address, sizeof(snap->ble_peer_address), "%s", "-");
    snprintf(snap->adapter_name, sizeof(snap->adapter_name), "%s", "MCP2515");

    obd_profile_t profile;
    memset(&profile, 0, sizeof(profile));
    if (profile_store_get_active(&profile) == ESP_OK) {
        snprintf(snap->obd_profile, sizeof(snap->obd_profile), "%s", profile.name);
    }
    can_obd_get_protocol(snap->obd_protocol, sizeof(snap->obd_protocol));

    snap->uptime_seconds = (uint32_t)sys_runtime_metric_get("uptime_s");
    bool can_ready = can_obd_is_ready();
    bool poller = obd_poller_is_enabled();
    snap->ble_connected = can_ready;
    snap->elm_ready = can_ready;
    snap->poller_status = poller ? "on" : "paused";
    snap->cmds_ok = sys_runtime_metric_get("cmds_ok");
    snap->cmds_fail = sys_runtime_metric_get("cmds_fail");
    snap->ble_reconnects = sys_runtime_metric_get("ble_reconnects");
    snap->blocked_cmds = sys_runtime_metric_get("blocked_cmds");
    snap->telemetry_drops = sys_runtime_metric_get("telemetry_drops");

    net_lte_gps_t gps;
    memset(&gps, 0, sizeof(gps));
    if (net_lte_gps_get(&gps) != ESP_OK) {
        gps.gps_ok = false;
    }
    snap->gps_ok = gps.gps_ok;
    snap->lat = gps.lat;
    snap->lng = gps.lng;

    return ESP_OK;
}

/** Producer: build snapshot → enqueue on SD (or live POST if no SD). */
static esp_err_t produce_once(void)
{
    uplink_snapshot_t snap;
    telemetry_uplink_config_t cfg;
    bool have_fresh = false;
    esp_err_t err = build_snapshot(&snap, &cfg, &have_fresh);
    if (err != ESP_OK) {
        return err;
    }

    if (!cfg.enabled) {
        set_last(false, true, 0, "disabled", "");
        return ESP_ERR_INVALID_STATE;
    }

    bool can_ready = can_obd_is_ready();
    bool poller = obd_poller_is_enabled();
    if (!can_ready || !poller) {
        set_last(false, true, 0, "not ready",
                 !can_ready ? "can link down" : "poller paused");
        return ESP_ERR_INVALID_STATE;
    }
    if (!have_fresh) {
        set_last(false, true, 0, "no fresh sample", "");
        return ESP_ERR_INVALID_STATE;
    }

    static char payload[PAYLOAD_BUF_LEN];
    int pn = uplink_payload_build_payload(&snap, payload, sizeof(payload));
    if (pn < 0) {
        set_last(false, true, 0, "json build fail", "");
        return ESP_FAIL;
    }

    if (store_sd_is_mounted()) {
        static char event[EVENT_BUF_LEN];
        int en = uplink_payload_build_queued_event(payload, now_ms(), event, sizeof(event));
        if (en < 0) {
            set_last(false, true, 0, "event build fail", "");
            return ESP_FAIL;
        }
        err = store_sd_enqueue_line(event, (size_t)en);
        if (err != ESP_OK) {
            set_last(false, false, 0, "enqueue fail", esp_err_to_name(err));
            ESP_LOGW(TAG, "enqueue fail: %s", esp_err_to_name(err));
            return err;
        }
        set_last(true, false, 0, "queued", "");
        ESP_LOGI(TAG, "enqueued %d bytes", en);
        if (s_drain_task) {
            xTaskNotifyGive(s_drain_task);
        }
        return ESP_OK;
    }

    /* No SD: legacy live single POST. */
    static char json[EVENT_BUF_LEN + 64];
    int n = uplink_payload_build(&snap, json, sizeof(json));
    if (n < 0) {
        set_last(false, true, 0, "json build fail", "");
        return ESP_FAIL;
    }
    if (fw_ota_lte_is_busy()) {
        set_last(false, true, 0, "ota busy", "");
        return ESP_ERR_INVALID_STATE;
    }
    net_lte_http_result_t http;
    err = net_lte_http_post(UPLINK_URL, json, &http);
    if (err == ESP_OK) {
        set_last(true, false, http.http_status, "posted", "");
        ESP_LOGI(TAG, "live POST ok status=%d bytes=%d", http.http_status, n);
    } else {
        set_last(false, false, http.http_status, "http fail", http.error);
        ESP_LOGW(TAG, "live POST fail: %s", http.error);
    }
    return err;
}

static esp_err_t drain_once(void)
{
    if (!store_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fw_ota_lte_is_busy()) {
        set_drain_err("ota busy");
        return ESP_ERR_INVALID_STATE;
    }

    store_sd_status_t st;
    store_sd_get_status(&st);
    if (st.depth == 0) {
        return ESP_OK;
    }

    static char peek[PEEK_BUF_LEN];
    size_t n_lines = 0;
    size_t span = 0;
    size_t max_ev = (size_t)CONFIG_STORE_SD_BATCH_MAX_EVENTS;
    esp_err_t err =
        store_sd_peek_lines(peek, sizeof(peek), max_ev, &n_lines, &span);
    if (err != ESP_OK || n_lines == 0) {
        return err == ESP_OK ? ESP_OK : err;
    }

    /* Shrink event count if batch body would exceed cap. */
    static char batch[BATCH_BUF_LEN];
    int bn = -1;
    size_t use_lines = n_lines;
    size_t use_span = span;
    while (use_lines > 0) {
        bn = uplink_payload_build_batch(peek, use_lines, batch, sizeof(batch));
        if (bn > 0 && (size_t)bn <= (size_t)CONFIG_STORE_SD_BATCH_MAX_BYTES) {
            break;
        }
        /* Drop last line from peek buffer and recompute span approx by re-peek. */
        use_lines--;
        if (use_lines == 0) {
            set_drain_err("event too large");
            return ESP_ERR_INVALID_SIZE;
        }
        err = store_sd_peek_lines(peek, sizeof(peek), use_lines, &use_lines, &use_span);
        if (err != ESP_OK || use_lines == 0) {
            return ESP_FAIL;
        }
    }
    if (bn <= 0) {
        set_drain_err("batch build fail");
        return ESP_FAIL;
    }

    net_lte_http_result_t http;
    err = net_lte_http_post(UPLINK_URL, batch, &http);
    if (err == ESP_OK) {
        esp_err_t ack = store_sd_ack_bytes(use_span, use_lines);
        set_drain_err("");
        set_last(true, false, http.http_status, "batch posted", "");
        ESP_LOGI(TAG, "batch POST ok n=%u bytes=%d ack=%s", (unsigned)use_lines, bn,
                 esp_err_to_name(ack));
        return ack;
    }

    set_drain_err(http.error[0] ? http.error : "http fail");
    set_last(false, false, http.http_status, "batch fail", http.error);
    ESP_LOGW(TAG, "batch POST fail: %s (queue kept)", http.error);
    return err;
}

static void tick_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "uplink produce task started");
    for (;;) {
        uint16_t iv = 5;
        if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(200)) == pdTRUE) {
            iv = s_cfg.interval_s;
            xSemaphoreGive(s_mu);
        }
        if (iv < 1) {
            iv = 1;
        }
        vTaskDelay(pdMS_TO_TICKS((uint32_t)iv * 1000u));
        bool enabled = false;
        if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(200)) == pdTRUE) {
            enabled = s_cfg.enabled;
            xSemaphoreGive(s_mu);
        }
        if (enabled) {
            produce_once();
        }
    }
}

static void drain_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "uplink drain task started");
    uint32_t backoff_ms = 2000;
    for (;;) {
        /* Wait for notify or poll periodically. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(backoff_ms));
        if (!store_sd_is_mounted()) {
            backoff_ms = 10000;
            continue;
        }
        if (fw_ota_lte_is_busy()) {
            backoff_ms = 5000;
            continue;
        }
        store_sd_status_t st;
        store_sd_get_status(&st);
        if (st.depth == 0) {
            backoff_ms = 5000;
            continue;
        }
        esp_err_t err = drain_once();
        if (err == ESP_OK) {
            backoff_ms = 1500;
            /* If more remain, drain again soon. */
            store_sd_get_status(&st);
            if (st.depth > 0) {
                xTaskNotifyGive(s_drain_task);
            }
        } else if (err == ESP_ERR_INVALID_STATE) {
            backoff_ms = 5000;
        } else {
            if (backoff_ms < 60000) {
                backoff_ms *= 2;
                if (backoff_ms > 60000) {
                    backoff_ms = 60000;
                }
            }
        }
    }
}

esp_err_t telemetry_uplink_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_mu = xSemaphoreCreateMutex();
    if (s_mu == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cfg_load();
    memset(&s_last, 0, sizeof(s_last));
    set_last(false, true, 0, "idle", "");
    set_drain_err("");

    esp_err_t err = telemetry_subscribe(&s_q, TELEMETRY_MASK_PID_SAMPLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "subscribe failed: %s", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(cache_task, "uplink_cache", UPLINK_CACHE_TASK_STACK, NULL, 3,
                    &s_cache_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(tick_task, "uplink_tick", UPLINK_TASK_STACK, NULL, UPLINK_TASK_PRIO,
                    &s_tick_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (store_sd_is_mounted()) {
        if (xTaskCreate(drain_task, "uplink_drain", UPLINK_DRAIN_STACK, NULL,
                        UPLINK_TASK_PRIO, &s_drain_task) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        ESP_LOGW(TAG, "SD not mounted — live POST fallback only");
    }
    s_started = true;
    ESP_LOGI(TAG, "started (enabled=%d interval=%us sd=%d)", s_cfg.enabled,
             s_cfg.interval_s, (int)store_sd_is_mounted());
    return ESP_OK;
}

esp_err_t telemetry_uplink_get_config(telemetry_uplink_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_cfg;
    xSemaphoreGive(s_mu);
    return ESP_OK;
}

esp_err_t telemetry_uplink_set_config(const telemetry_uplink_config_t *in)
{
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (in->device_id[0] == '\0' || in->node_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    telemetry_uplink_config_t c = *in;
    if (c.interval_s < 1) {
        c.interval_s = 1;
    }
    if (c.interval_s > 300) {
        c.interval_s = 300;
    }
    esp_err_t err = cfg_save(&c);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_cfg = c;
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "config saved enabled=%d interval=%u", c.enabled, c.interval_s);
    return ESP_OK;
}

esp_err_t telemetry_uplink_get_status(telemetry_uplink_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    out->config = s_cfg;
    out->last = s_last;
    snprintf(out->queue.drain_error, sizeof(out->queue.drain_error), "%s", s_drain_err);
    xSemaphoreGive(s_mu);
    out->url = UPLINK_URL;
    out->schema_id = UPLINK_SCHEMA_ID;

    store_sd_status_t sd;
    memset(&sd, 0, sizeof(sd));
    store_sd_get_status(&sd);
    out->queue.sd_mounted = sd.mounted;
    out->queue.queue_depth = sd.depth;
    out->queue.queue_bytes = sd.pending_bytes;
    return ESP_OK;
}

esp_err_t telemetry_uplink_send_now(void)
{
    esp_err_t err = produce_once();
    if (store_sd_is_mounted() && s_drain_task) {
        xTaskNotifyGive(s_drain_task);
        /* Best-effort one drain attempt for interactive "now". */
        vTaskDelay(pdMS_TO_TICKS(50));
        drain_once();
    }
    return err;
}

esp_err_t telemetry_uplink_queue_test_enqueue(void)
{
    if (!store_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    char line[192];
    int n = snprintf(line, sizeof(line),
                     "{\"payload\":{\"device_id\":\"qtest\",\"node_id\":\"esp32c6\","
                     "\"schema_version\":1,\"source\":\"esp32_obd\"},\"queued_at_ms\":%llu}",
                     (unsigned long long)now_ms());
    if (n <= 0 || (size_t)n >= sizeof(line)) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = store_sd_enqueue_line(line, (size_t)n);
    if (err != ESP_OK) {
        return err;
    }
    set_last(true, false, 0, "qtest queued", "");
    if (s_drain_task) {
        xTaskNotifyGive(s_drain_task);
    }
    return ESP_OK;
}
