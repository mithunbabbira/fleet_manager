#include "telemetry_uplink.h"
#include "uplink_payload.h"
#include "uplink_schema.h"

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

/*
 * LTE telemetry uplink orchestrator.
 *
 * Why two paths?
 * - With SD: produce always enqueues (offline-safe); drain POSTs batches so a
 *   long outage does not lose samples and a single POST stays under size caps.
 * - Without SD: produce POSTs one event immediately (lab / no-card fallback).
 *
 * HTTP target is UPLINK_URL in uplink_payload.h (Trafyn nc-events-api).
 * OTA download shares the modem UART — skip POST while fw_ota_lte is busy.
 */

static const char *TAG = "uplink";

/* NVS namespace "elm" — survives OTA; SoftAP/serial edit these, not the POST URL. */
#define NVS_NS "elm"
#define NVS_KEY_EN "uplink_en"   /* 0/1 enable */
#define NVS_KEY_IV "uplink_iv"   /* produce interval seconds */
#define NVS_KEY_DID "uplink_did" /* Trafyn/deviceId (shared with fw_ota_lte) */
#define NVS_KEY_NID "uplink_nid" /* node_id in payload */
#define NVS_KEY_POST_URL "uplink_url"
#define NVS_KEY_SCHEMA "uplink_schema"

#define UPLINK_TASK_STACK 8192
#define UPLINK_DRAIN_STACK 8192
#define UPLINK_TASK_PRIO 4
#define UPLINK_CACHE_TASK_STACK 3072
#define EVENT_BUF_LEN 1024
#define LIVE_BUF_LEN 8192
#define BATCH_BUF_LEN 13000
#define PEEK_BUF_LEN 12000

#ifndef CONFIG_STORE_SD_BATCH_MAX_EVENTS
#define CONFIG_STORE_SD_BATCH_MAX_EVENTS 10
#endif
#ifndef CONFIG_STORE_SD_BATCH_MAX_BYTES
#define CONFIG_STORE_SD_BATCH_MAX_BYTES 12288
#endif

static telemetry_uplink_config_t s_cfg;
static char s_post_url[256];
static char s_schema_id[16];
static telemetry_uplink_last_t s_last;
static char s_drain_err[80];
static SemaphoreHandle_t s_mu;
/* Serializes produce/drain (shared static JSON buffers + modem access). */
static SemaphoreHandle_t s_io_mu;
static QueueHandle_t s_q;
static TaskHandle_t s_cache_task;
static TaskHandle_t s_tick_task;
static TaskHandle_t s_drain_task;
static bool s_started;
static bool s_have_gps_last;
static double s_last_gps_lat;
static double s_last_gps_lng;
static uint64_t s_last_gps_ms;

static telemetry_pid_sample_t s_rpm;
static telemetry_pid_sample_t s_speed;
static telemetry_pid_sample_t s_coolant;
static telemetry_pid_sample_t s_throttle;
static telemetry_pid_sample_t s_voltage;
static bool s_have_rpm, s_have_speed, s_have_coolant, s_have_throttle, s_have_voltage;

static uplink_host_report_t s_hosts[UPLINK_MAX_HOSTS];
/* Carrier-side receive time per host; host ts_ms uses the host's own clock. */
static uint64_t s_host_rx_ms[UPLINK_MAX_HOSTS];
static uint8_t s_host_count;

/* Stop uploading a host's readings once it goes quiet this long. */
#define UPLINK_HOST_FRESH_MS 120000ULL

/**
 * @brief Monotonic ms since boot.
 * @note All age/freshness math must use this — telemetry_bus samples are
 *       stamped with the same clock, and it never jumps when time syncs.
 */
static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

/**
 * @brief Timestamp for outgoing events: UTC epoch ms once the modem has time,
 *        otherwise uptime ms.
 */
static uint64_t event_ts_ms(void)
{
    uint64_t wall = net_lte_time_now_ms();
    return wall != 0 ? wall : now_ms();
}

/** @brief Update GPS-only dedup last lat/lng/time when snap has fix. */
static void remember_gps_if_ok(const uplink_snapshot_t *snap)
{
    if (!snap->gps_ok) {
        return;
    }
    s_have_gps_last = true;
    s_last_gps_lat = snap->lat;
    s_last_gps_lng = snap->lng;
    s_last_gps_ms = snap->ts_ms;
}

/** @brief Record last produce/drain diagnostic (SoftAP/serial). */
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
    c->enabled = false;
    c->interval_s = 60;
    /* device_id / node_id intentionally empty until Carrier Console provision. */
}

static void endpoint_defaults(void)
{
    snprintf(s_post_url, sizeof(s_post_url), "%s", UPLINK_URL);
    snprintf(s_schema_id, sizeof(s_schema_id), "%s", UPLINK_SCHEMA_ID);
}

static bool cfg_is_provisioned(const telemetry_uplink_config_t *c)
{
    return c != NULL && c->device_id[0] != '\0' && c->node_id[0] != '\0';
}

bool telemetry_uplink_is_provisioned(void)
{
    bool ok;
    if (s_mu != NULL && xSemaphoreTake(s_mu, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ok = cfg_is_provisioned(&s_cfg);
        xSemaphoreGive(s_mu);
    } else {
        ok = cfg_is_provisioned(&s_cfg);
    }
    return ok;
}

static esp_err_t cfg_load(void)
{
    cfg_defaults(&s_cfg);
    endpoint_defaults();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    uint8_t en = 0;
    if (nvs_get_u8(h, NVS_KEY_EN, &en) == ESP_OK) {
        s_cfg.enabled = en != 0;
    }
    uint16_t iv = 60;
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
    len = sizeof(s_post_url);
    if (nvs_get_str(h, NVS_KEY_POST_URL, s_post_url, &len) != ESP_OK || s_post_url[0] == '\0') {
        endpoint_defaults();
    }
    len = sizeof(s_schema_id);
    if (nvs_get_str(h, NVS_KEY_SCHEMA, s_schema_id, &len) != ESP_OK || s_schema_id[0] == '\0') {
        snprintf(s_schema_id, sizeof(s_schema_id), "%s", UPLINK_SCHEMA_ID);
    }
    nvs_close(h);
    if (!cfg_is_provisioned(&s_cfg)) {
        s_cfg.enabled = false;
    }
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

static esp_err_t endpoint_save_url(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_POST_URL, url);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        snprintf(s_post_url, sizeof(s_post_url), "%s", url);
    }
    return err;
}

static esp_err_t endpoint_save_schema(const char *schema)
{
    if (schema == NULL || schema[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_KEY_SCHEMA, schema);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        snprintf(s_schema_id, sizeof(s_schema_id), "%s", schema);
    }
    return err;
}

esp_err_t telemetry_uplink_provision(const char *device_id, const char *node_id)
{
    if (device_id == NULL || device_id[0] == '\0' || node_id == NULL || node_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    telemetry_uplink_config_t c;
    if (telemetry_uplink_get_config(&c) != ESP_OK) {
        return ESP_FAIL;
    }
    snprintf(c.device_id, sizeof(c.device_id), "%s", device_id);
    snprintf(c.node_id, sizeof(c.node_id), "%s", node_id);
    esp_err_t err = telemetry_uplink_set_config(&c);
    if (err != ESP_OK) {
        return err;
    }
    fw_ota_lte_config_t ota;
    if (fw_ota_lte_get_config(&ota) == ESP_OK) {
        snprintf(ota.device_id, sizeof(ota.device_id), "%s", device_id);
        (void)fw_ota_lte_set_config(&ota);
    }
    return ESP_OK;
}

esp_err_t telemetry_uplink_set_post_url(const char *url)
{
    return endpoint_save_url(url);
}

esp_err_t telemetry_uplink_set_schema_id(const char *schema_id)
{
    return endpoint_save_schema(schema_id);
}

/** @brief Cache rpm/speed/coolant/throttle/voltage samples from bus. */
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

static void store_host_report(const telemetry_host_report_t *rep)
{
    if (rep == NULL || rep->device_id[0] == '\0') {
        return;
    }
    for (uint8_t i = 0; i < s_host_count; i++) {
        if (strcmp(s_hosts[i].device_id, rep->device_id) == 0) {
            s_host_rx_ms[i] = now_ms();
            s_hosts[i].host_type_id = rep->host_type_id;
            snprintf(s_hosts[i].host_type, sizeof(s_hosts[i].host_type), "%s", rep->host_type);
            s_hosts[i].ts_ms = rep->ts_ms;
            s_hosts[i].reading_count = rep->reading_count;
            if (s_hosts[i].reading_count > UPLINK_MAX_HOST_READINGS) {
                s_hosts[i].reading_count = UPLINK_MAX_HOST_READINGS;
            }
            for (uint8_t r = 0; r < s_hosts[i].reading_count; r++) {
                snprintf(s_hosts[i].readings[r].key, sizeof(s_hosts[i].readings[r].key), "%s",
                         rep->readings[r].key);
                snprintf(s_hosts[i].readings[r].unit, sizeof(s_hosts[i].readings[r].unit), "%s",
                         rep->readings[r].unit);
                s_hosts[i].readings[r].value = rep->readings[r].value;
                s_hosts[i].readings[r].valid = rep->readings[r].valid;
            }
            return;
        }
    }
    uint8_t slot;
    if (s_host_count >= UPLINK_MAX_HOSTS) {
        /* Full: reuse the least-recently-heard slot instead of ignoring the
         * new host until reboot. */
        slot = 0;
        for (uint8_t i = 1; i < UPLINK_MAX_HOSTS; i++) {
            if (s_host_rx_ms[i] < s_host_rx_ms[slot]) {
                slot = i;
            }
        }
    } else {
        slot = s_host_count++;
    }
    s_host_rx_ms[slot] = now_ms();
    uplink_host_report_t *h = &s_hosts[slot];
    memset(h, 0, sizeof(*h));
    snprintf(h->device_id, sizeof(h->device_id), "%s", rep->device_id);
    snprintf(h->host_type, sizeof(h->host_type), "%s", rep->host_type);
    h->host_type_id = rep->host_type_id;
    h->ts_ms = rep->ts_ms;
    h->reading_count = rep->reading_count;
    if (h->reading_count > UPLINK_MAX_HOST_READINGS) {
        h->reading_count = UPLINK_MAX_HOST_READINGS;
    }
    for (uint8_t r = 0; r < h->reading_count; r++) {
        snprintf(h->readings[r].key, sizeof(h->readings[r].key), "%s", rep->readings[r].key);
        snprintf(h->readings[r].unit, sizeof(h->readings[r].unit), "%s", rep->readings[r].unit);
        h->readings[r].value = rep->readings[r].value;
        h->readings[r].valid = rep->readings[r].valid;
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
        if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (msg.type == TELEMETRY_PID_SAMPLE) {
            store_sample(&msg.pid_sample);
        } else if (msg.type == TELEMETRY_HOST_REPORT) {
            store_host_report(&msg.host_report);
        }
        xSemaphoreGive(s_mu);
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

/** @brief Any PID fresh within UPLINK_PID_FRESH_MS (caller holds s_mu). */
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

/** @brief Snapshot: cached PIDs + GNSS cache + ids/metrics. */
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
    /* Only carry hosts still reporting — a dead host must not keep re-uploading
     * its last reading every tick. */
    snap->host_count = 0;
    for (uint8_t i = 0; i < s_host_count && i < UPLINK_MAX_HOSTS; i++) {
        uint64_t rx = s_host_rx_ms[i];
        if (rx == 0 || now < rx || (now - rx) > UPLINK_HOST_FRESH_MS) {
            continue;
        }
        snap->hosts[snap->host_count++] = s_hosts[i];
    }
    xSemaphoreGive(s_mu);

    obd_profile_t profile;
    memset(&profile, 0, sizeof(profile));
    if (profile_store_get_active(&profile) == ESP_OK) {
        snprintf(snap->obd_profile, sizeof(snap->obd_profile), "%s", profile.name);
    }
    can_obd_get_protocol(snap->obd_protocol, sizeof(snap->obd_protocol));

    snap->uptime_seconds = (uint32_t)sys_runtime_metric_get("uptime_s");
    bool poller = obd_poller_is_enabled();
    snap->poller_status = poller ? "on" : "paused";
    snap->cmds_ok = sys_runtime_metric_get("cmds_ok");
    snap->cmds_fail = sys_runtime_metric_get("cmds_fail");
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

/**
 * @brief Produce tick: gates → JSON → SD enqueue or live POST.
 * @note OTA busy only blocks live modem POST; SD enqueue still runs so samples
 *       accumulate while LTE OTA holds the UART.
 * @note Callers must hold s_io_mu (see produce_once).
 */
static esp_err_t produce_once_locked(void)
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

    if (!cfg_is_provisioned(&cfg)) {
        set_last(false, true, 0, "not provisioned", "");
        return ESP_ERR_INVALID_STATE;
    }

    snap.ts_ms = event_ts_ms();

    bool gps_worth = true;
    if (!have_fresh && snap.gps_ok) {
        gps_worth = uplink_gps_only_worth_sending(s_have_gps_last, s_last_gps_lat, s_last_gps_lng,
                                                  s_last_gps_ms, snap.lat, snap.lng, snap.ts_ms);
    }
    if (!uplink_tick_worth_producing(have_fresh, snap.gps_ok, gps_worth, &snap)) {
        set_last(false, true, 0, "no events", "");
        return ESP_ERR_INVALID_STATE;
    }

    uplink_emit_ctx_t emit = {
        .include_obd = have_fresh,
        .include_gps = snap.gps_ok && (have_fresh || gps_worth),
        .obd_schema_id = s_schema_id,
    };
    /* Static: ~10 KB would blow the task stack. Serialized by s_io_mu. */
    static uplink_event_t events[UPLINK_MAX_EVENTS_PER_TICK];
    uint8_t event_count = 0;
    if (uplink_events_from_snapshot(&snap, &emit, events, UPLINK_MAX_EVENTS_PER_TICK,
                                    &event_count) != 0 ||
        event_count == 0) {
        set_last(false, true, 0, "json build fail", "");
        return ESP_FAIL;
    }

    if (store_sd_is_mounted()) {
        static char event[EVENT_BUF_LEN];
        uint64_t queued_at = now_ms();
        uint8_t queued = 0;
        bool enqueue_failed = false;
        for (uint8_t i = 0; i < event_count; i++) {
            int en =
                uplink_event_serialize_queued(&events[i], queued_at, event, sizeof(event));
            if (en < 0) {
                ESP_LOGW(TAG, "skipped oversized event %u", (unsigned)i);
                continue;
            }
            if (store_sd_enqueue_line(event, (size_t)en) != ESP_OK) {
                enqueue_failed = true;
                break;
            }
            queued++;
        }
        if (queued > 0) {
            set_last(true, false, 0, "queued", "");
            ESP_LOGI(TAG, "enqueued %u events", (unsigned)queued);
            remember_gps_if_ok(&snap);
            if (s_drain_task) {
                xTaskNotifyGive(s_drain_task);
            }
            return ESP_OK;
        }
        if (!enqueue_failed) {
            set_last(false, true, 0, "event build fail", "");
            return ESP_FAIL;
        }
        /* Card failed this tick — fall through to live POST rather than lose
         * the sample. store_sd unmounts itself after repeated write errors. */
        ESP_LOGW(TAG, "SD enqueue failed — trying live POST");
    }

    /*
     * No usable SD: live POST to UPLINK_URL.
     * Body is one envelope or a JSON array (see uplink_events_serialize_live).
     */
    static char json[LIVE_BUF_LEN];
    int n = uplink_events_serialize_live(events, event_count, json, sizeof(json));
    if (n < 0) {
        set_last(false, true, 0, "json build fail", "");
        return ESP_FAIL;
    }
    if (fw_ota_lte_is_busy()) {
        set_last(false, true, 0, "ota busy", "");
        return ESP_ERR_INVALID_STATE;
    }
    net_lte_http_result_t http;
    err = net_lte_http_post(s_post_url, json, &http);
    if (err == ESP_OK) {
        set_last(true, false, http.http_status, "posted", "");
        ESP_LOGI(TAG, "live POST ok status=%d bytes=%d", http.http_status, n);
        remember_gps_if_ok(&snap);
    } else {
        set_last(false, false, http.http_status, "http fail", http.error);
        ESP_LOGW(TAG, "live POST fail: %s", http.error);
    }
    return err;
}

/**
 * @brief Peek SD batch, POST array; ack bytes only on HTTP 2xx (400 keeps head).
 * @note Callers must hold s_io_mu (see drain_once).
 */
static esp_err_t drain_once_locked(void)
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
        if (use_lines == 1) {
            /* One record that can never be sent (legacy format, corrupt, or
             * oversized). Drop it so the queue always moves forward. */
            (void)store_sd_ack_bytes(use_span, 1);
            set_drain_err("dropped unusable record");
            ESP_LOGW(TAG, "dropped unusable queue record (%u bytes)", (unsigned)use_span);
            return ESP_OK;
        }
        /* Drop last line from peek buffer and recompute span approx by re-peek. */
        use_lines--;
        err = store_sd_peek_lines(peek, sizeof(peek), use_lines, &use_lines, &use_span);
        if (err != ESP_OK || use_lines == 0) {
            return ESP_FAIL;
        }
    }
    if (bn <= 0) {
        set_drain_err("batch build fail");
        return ESP_FAIL;
    }

    /*
     * Batch POST to the same UPLINK_URL as live path.
     * Body is a JSON array of envelopes (schemaId per event).
     * On HTTP failure keep SD lines (ack only after success) so samples retry.
     */
    net_lte_http_result_t http;
    err = net_lte_http_post(s_post_url, batch, &http);
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

/*
 * produce and drain share large static buffers and the modem, and can be
 * invoked from the tick task, the drain task, and `uplink now` at the same
 * time. One gate keeps them strictly serialized.
 */
static esp_err_t produce_once(void)
{
    if (s_io_mu == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_io_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        set_last(false, true, 0, "busy", "");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = produce_once_locked();
    xSemaphoreGive(s_io_mu);
    return err;
}

static esp_err_t drain_once(void)
{
    if (s_io_mu == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_io_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = drain_once_locked();
    xSemaphoreGive(s_io_mu);
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

/** @brief Wait notify/backoff; skip if OTA busy; drain_once with backoff. */
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
    s_io_mu = xSemaphoreCreateMutex();
    if (s_io_mu == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cfg_load();
    memset(&s_last, 0, sizeof(s_last));
    set_last(false, true, 0, "idle", "");
    set_drain_err("");

    esp_err_t err =
        telemetry_subscribe(&s_q, TELEMETRY_MASK_PID_SAMPLE | TELEMETRY_MASK_HOST_REPORT);
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
    ESP_LOGI(TAG, "started (enabled=%d interval=%us sd=%d provisioned=%d)", s_cfg.enabled,
             s_cfg.interval_s, (int)store_sd_is_mounted(), (int)cfg_is_provisioned(&s_cfg));
    if (!cfg_is_provisioned(&s_cfg)) {
        ESP_LOGW(TAG, "device not provisioned — set device_id + node_id via Carrier Console");
    }
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
    if (c.enabled && !cfg_is_provisioned(&c)) {
        return ESP_ERR_INVALID_STATE;
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
    out->url = s_post_url;
    out->schema_id = s_schema_id;

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
    if (!telemetry_uplink_is_provisioned()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!store_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    telemetry_uplink_config_t cfg;
    if (telemetry_uplink_get_config(&cfg) != ESP_OK) {
        return ESP_FAIL;
    }
    char line[256];
    uint64_t ts = event_ts_ms();
    uplink_event_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.device_id, sizeof(ev.device_id), "%s", cfg.device_id);
    snprintf(ev.node_id, sizeof(ev.node_id), "%s", cfg.node_id);
    ev.schema_id = UPLINK_SCHEMA_OBD;
    ev.ts_ms = ts;
    snprintf(ev.payload_json, sizeof(ev.payload_json), "{\"source\":\"esp32_obd\"}");
    int n = uplink_event_serialize_queued(&ev, ts, line, sizeof(line));
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
