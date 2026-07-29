#include "telemetry_uplink.h"
#include "uplink_payload.h"

#include "ble_elm.h"
#include "elm327_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_lte.h"
#include "nvs.h"
#include "obd_poller.h"
#include "profile_store.h"
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
#define UPLINK_TASK_PRIO 4
#define UPLINK_CACHE_TASK_STACK 3072
#define JSON_BUF_LEN 2048

static telemetry_uplink_config_t s_cfg;
static telemetry_uplink_last_t s_last;
static SemaphoreHandle_t s_mu;
static QueueHandle_t s_q;
static TaskHandle_t s_cache_task;
static TaskHandle_t s_tick_task;
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

static void cfg_defaults(telemetry_uplink_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->enabled = false;
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
        return ESP_OK; /* first boot */
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

static void format_addr(const uint8_t addr[6], char *out, size_t len)
{
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static esp_err_t attempt_once(void)
{
    telemetry_uplink_config_t cfg;
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    cfg = s_cfg;
    uint64_t now = now_ms();
    bool have_fresh = any_fresh_ok_locked(now);

    uplink_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snprintf(snap.device_id, sizeof(snap.device_id), "%s", cfg.device_id);
    snprintf(snap.node_id, sizeof(snap.node_id), "%s", cfg.node_id);
    fill_pid_view(&snap.rpm, s_have_rpm, &s_rpm, now);
    fill_pid_view(&snap.speed, s_have_speed, &s_speed, now);
    fill_pid_view(&snap.coolant, s_have_coolant, &s_coolant, now);
    fill_pid_view(&snap.throttle, s_have_throttle, &s_throttle, now);
    fill_pid_view(&snap.voltage, s_have_voltage, &s_voltage, now);
    xSemaphoreGive(s_mu);

    if (!cfg.enabled) {
        set_last(false, true, 0, "disabled", "");
        return ESP_ERR_INVALID_STATE;
    }

    bool ble = ble_elm_is_connected();
    bool elm = elm327_client_is_ready();
    bool poller = obd_poller_is_enabled();
    if (!ble || !elm || !poller) {
        set_last(false, true, 0, "not ready",
                 !ble ? "ble down" : (!elm ? "elm not ready" : "poller paused"));
        return ESP_ERR_INVALID_STATE;
    }
    if (!have_fresh) {
        set_last(false, true, 0, "no fresh sample", "");
        return ESP_ERR_INVALID_STATE;
    }

    ble_bond_t bond;
    memset(&bond, 0, sizeof(bond));
    profile_store_get_bond(&bond);
    if (bond.addr_set) {
        format_addr(bond.addr, snap.ble_peer_address, sizeof(snap.ble_peer_address));
    } else {
        uint8_t peer[6] = {0};
        if (ble_elm_get_peer_addr(peer) == ESP_OK) {
            format_addr(peer, snap.ble_peer_address, sizeof(snap.ble_peer_address));
        }
    }
    snprintf(snap.adapter_name, sizeof(snap.adapter_name), "%s",
             bond.name[0] ? bond.name : "ELM327");

    obd_profile_t profile;
    memset(&profile, 0, sizeof(profile));
    if (profile_store_get_active(&profile) == ESP_OK) {
        snprintf(snap.obd_profile, sizeof(snap.obd_profile), "%s", profile.name);
    }
    snprintf(snap.obd_protocol, sizeof(snap.obd_protocol), "%s", "unknown");

    snap.uptime_seconds = (uint32_t)sys_runtime_metric_get("uptime_s");
    snap.ble_connected = ble;
    snap.elm_ready = elm;
    snap.poller_status = poller ? "on" : "paused";
    snap.cmds_ok = sys_runtime_metric_get("cmds_ok");
    snap.cmds_fail = sys_runtime_metric_get("cmds_fail");
    snap.ble_reconnects = sys_runtime_metric_get("ble_reconnects");
    snap.blocked_cmds = sys_runtime_metric_get("blocked_cmds");
    snap.telemetry_drops = sys_runtime_metric_get("telemetry_drops");

    static char json[JSON_BUF_LEN];
    int n = uplink_payload_build(&snap, json, sizeof(json));
    if (n < 0) {
        set_last(false, true, 0, "json build fail", "");
        return ESP_FAIL;
    }

    net_lte_http_result_t http;
    esp_err_t err = net_lte_http_post(UPLINK_URL, json, &http);
    if (err == ESP_OK) {
        set_last(true, false, http.http_status, "posted", "");
        ESP_LOGI(TAG, "POST ok status=%d bytes=%d", http.http_status, n);
    } else {
        set_last(false, false, http.http_status, "http fail", http.error);
        ESP_LOGW(TAG, "POST fail: %s (%s)", http.error, esp_err_to_name(err));
    }
    return err;
}

static void tick_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "uplink tick task started");
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
            attempt_once();
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
    s_started = true;
    ESP_LOGI(TAG, "started (enabled=%d interval=%us)", s_cfg.enabled, s_cfg.interval_s);
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
    xSemaphoreGive(s_mu);
    out->url = UPLINK_URL;
    out->schema_id = UPLINK_SCHEMA_ID;
    return ESP_OK;
}

esp_err_t telemetry_uplink_send_now(void)
{
    return attempt_once();
}
