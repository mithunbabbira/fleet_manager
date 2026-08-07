#include "http_api.h"

#include "static_index.html.h"

#include "can_obd.h"
#include "cmd_policy.h"
#include "fw_ota.h"
#include "fw_ota_lte.h"
#include "net_lte.h"
#include "obd_codec.h"
#include "obd_poller.h"
#include "profile_store.h"
#include "sys_runtime.h"
#include "telemetry_bus.h"
#include "telemetry_uplink.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "http_api";

#define REQ_BODY_BUF_LEN     512
#define PROFILE_BODY_BUF_LEN 4096
#define RESP_BUF_LEN         256
#define METRICS_BUF_LEN      768
#define MAX_PROFILE_NAMES    16

#define TELE_CACHE_CAP        8
#define TELE_CACHE_TASK_STACK 3072
#define TELE_CACHE_TASK_PRIO  3

/* ---- telemetry cache (subscriber task feeds GET /api/telemetry) -------- */

typedef struct {
    telemetry_pid_sample_t items[TELE_CACHE_CAP];
    int count;
    int next;
} pid_ring_t;

static SemaphoreHandle_t s_cache_mutex;
static QueueHandle_t s_cache_queue;
static TaskHandle_t s_cache_task_handle;

static pid_ring_t s_pid_ring;
static telemetry_elm_event_t s_last_event;
static bool s_have_event;
static telemetry_error_msg_t s_last_error;
static bool s_have_error;
static telemetry_dtc_list_t s_last_dtc;
static bool s_have_dtc;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void pid_ring_push_locked(const telemetry_pid_sample_t *s)
{
    s_pid_ring.items[s_pid_ring.next] = *s;
    s_pid_ring.next = (s_pid_ring.next + 1) % TELE_CACHE_CAP;
    if (s_pid_ring.count < TELE_CACHE_CAP) {
        s_pid_ring.count++;
    }
}

static void telemetry_cache_task(void *arg)
{
    (void)arg;
    telemetry_msg_t msg;

    ESP_LOGI(TAG, "telemetry cache task started");
    for (;;) {
        if (xQueueReceive(s_cache_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
        switch (msg.type) {
        case TELEMETRY_PID_SAMPLE:
            pid_ring_push_locked(&msg.pid_sample);
            break;
        case TELEMETRY_ELM_EVENT:
            s_last_event = msg.elm_event;
            s_have_event = true;
            break;
        case TELEMETRY_ERROR:
            s_last_error = msg.error;
            s_have_error = true;
            break;
        case TELEMETRY_DTC_LIST:
            s_last_dtc = msg.dtc_list;
            s_have_dtc = true;
            break;
        default:
            break;
        }
        xSemaphoreGive(s_cache_mutex);
    }
}

static esp_err_t telemetry_cache_start(void)
{
    if (s_cache_task_handle != NULL) {
        return ESP_OK;
    }

    s_cache_mutex = xSemaphoreCreateMutex();
    if (s_cache_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = telemetry_subscribe(&s_cache_queue, TELEMETRY_MASK_ALL);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t created = xTaskCreate(telemetry_cache_task, "http_tele_cache",
                                     TELE_CACHE_TASK_STACK, NULL, TELE_CACHE_TASK_PRIO,
                                     &s_cache_task_handle);
    if (created != pdPASS) {
        s_cache_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* ---- small helpers ------------------------------------------------------ */

/* Reads the full request body into `buf` (NUL-terminated). Fails with
 * ESP_ERR_INVALID_SIZE if the body does not fit. */
static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len == 0) {
        buf[0] = '\0';
        return ESP_OK;
    }
    if (req->content_len >= buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, const char *http_status, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_status(req, http_status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}

static esp_err_t send_ok_json(httpd_req_t *req, cJSON *root)
{
    return send_json(req, HTTPD_200, root);
}

static esp_err_t send_error_json(httpd_req_t *req, const char *http_status, const char *error,
                                  const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", error ? error : "error");
    if (message) {
        cJSON_AddStringToObject(root, "message", message);
    }
    return send_json(req, http_status, root);
}

static void attach_metrics(cJSON *root)
{
    char buf[METRICS_BUF_LEN];
    sys_runtime_metrics_snapshot_json(buf, sizeof(buf));
    cJSON *metrics = cJSON_Parse(buf);
    cJSON_AddItemToObject(root, "metrics", metrics ? metrics : cJSON_CreateObject());
}

static void attach_decoded(cJSON *root, const obd_decoded_t *d)
{
    cJSON *dec = cJSON_CreateObject();
    cJSON_AddStringToObject(dec, "name", d->name ? d->name : "");
    cJSON_AddStringToObject(dec, "unit", d->unit ? d->unit : "");
    cJSON_AddNumberToObject(dec, "value", d->value);
    cJSON_AddStringToObject(dec, "raw_hex", d->raw_hex);
    cJSON_AddItemToObject(root, "decoded", dec);
}

/* Best-effort decode of an already-successful raw ELM response, based on
 * the normalized command text. Mirrors the decode keys used by profiles
 * (obd_poller/builtin_profiles) but works generically for any mode-01 PID
 * so ad-hoc commands sent from the UI still get a decoded value. */
static void try_decode(const char *norm_cmd, const char *resp, cJSON *root)
{
    obd_decoded_t d;

    if (strcmp(norm_cmd, "ATRV") == 0) {
        if (obd_codec_decode_named("voltage", resp, &d) && d.ok) {
            attach_decoded(root, &d);
        }
        return;
    }

    size_t len = strlen(norm_cmd);
    if (len >= 4 && norm_cmd[0] == '0' && norm_cmd[1] == '1') {
        unsigned pid_val = 0;
        if (sscanf(norm_cmd + 2, "%2x", &pid_val) == 1 &&
            obd_codec_decode_mode01(resp, (uint8_t)pid_val, &d) && d.ok) {
            attach_decoded(root, &d);
        }
        return;
    }

    if (strcmp(norm_cmd, "03") == 0 || strcmp(norm_cmd, "07") == 0 ||
        strcmp(norm_cmd, "0A") == 0) {
        char dtcs[8][6];
        int n = obd_codec_parse_dtcs(resp, dtcs, 8);
        if (n > 0) {
            cJSON *arr = cJSON_CreateArray();
            for (int i = 0; i < n; ++i) {
                cJSON_AddItemToArray(arr, cJSON_CreateString(dtcs[i]));
            }
            cJSON_AddItemToObject(root, "dtcs", arr);
        }
        return;
    }

    if (strcmp(norm_cmd, "0902") == 0) {
        char vin[18];
        if (obd_codec_parse_vin(resp, vin, sizeof(vin))) {
            cJSON_AddStringToObject(root, "vin", vin);
        }
    }
}

/* ---- obd_profile_t <-> cJSON (mirrors profile_store's private mapping) - */

static esp_err_t parse_profile_json(const cJSON *root, obd_profile_t *out)
{
    memset(out, 0, sizeof(*out));

    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(name) || !name->valuestring || !name->valuestring[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(out->name, sizeof(out->name), "%s", name->valuestring);

    cJSON *init_at = cJSON_GetObjectItem(root, "init_at");
    if (cJSON_IsArray(init_at)) {
        int n = cJSON_GetArraySize(init_at);
        for (int i = 0; i < n && out->init_at_count < 8; ++i) {
            cJSON *item = cJSON_GetArrayItem(init_at, i);
            if (cJSON_IsString(item) && item->valuestring) {
                snprintf(out->init_at[out->init_at_count], sizeof(out->init_at[0]), "%s",
                         item->valuestring);
                out->init_at_count++;
            }
        }
    }

    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(items)) {
        int n = cJSON_GetArraySize(items);
        for (int i = 0; i < n && out->item_count < 32; ++i) {
            cJSON *item = cJSON_GetArrayItem(items, i);
            if (!cJSON_IsObject(item)) {
                continue;
            }
            profile_item_t *dst = &out->items[out->item_count];
            cJSON *cmd = cJSON_GetObjectItem(item, "cmd");
            cJSON *interval = cJSON_GetObjectItem(item, "interval_ms");
            cJSON *decode = cJSON_GetObjectItem(item, "decode");
            if (!cJSON_IsString(cmd) || !cmd->valuestring) {
                continue;
            }
            snprintf(dst->cmd, sizeof(dst->cmd), "%s", cmd->valuestring);
            if (cJSON_IsNumber(interval)) {
                dst->interval_ms = (uint32_t)interval->valuedouble;
            }
            if (cJSON_IsString(decode) && decode->valuestring) {
                snprintf(dst->decode, sizeof(dst->decode), "%s", decode->valuestring);
            }
            out->item_count++;
        }
    }

    return ESP_OK;
}

/* ---- GET /api/status ----------------------------------------------------- */

static esp_err_t api_status_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /api/status");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "can_ready", can_obd_is_ready());

    char protocol[48];
    can_obd_get_protocol(protocol, sizeof(protocol));
    cJSON_AddStringToObject(root, "obd_protocol", protocol);

    cJSON_AddBoolToObject(root, "poller_enabled", obd_poller_is_enabled());

    obd_profile_t active;
    if (profile_store_get_active(&active) == ESP_OK) {
        cJSON_AddStringToObject(root, "active_profile", active.name);
    } else {
        cJSON_AddNullToObject(root, "active_profile");
    }

    cJSON_AddNumberToObject(root, "uptime_s",
                            (double)sys_runtime_metric_get("uptime_s"));
    attach_metrics(root);

    return send_ok_json(req, root);
}

/* ---- POST /api/obd/cmd ------------------------------------------------------- */

static esp_err_t api_obd_cmd_post(httpd_req_t *req)
{
    if (!can_obd_is_ready()) {
        return send_error_json(req, "503 Service Unavailable", "not_ready",
                               "CAN link not ready — check MCP2515 wiring / ignition");
    }

    char body[REQ_BODY_BUF_LEN];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return send_error_json(req, HTTPD_400, "bad_request", "could not read body");
    }

    cJSON *json = cJSON_Parse(body);
    cJSON *cmd_item = json ? cJSON_GetObjectItem(json, "cmd") : NULL;
    if (!cJSON_IsString(cmd_item) || !cmd_item->valuestring || !cmd_item->valuestring[0]) {
        if (json) {
            cJSON_Delete(json);
        }
        return send_error_json(req, HTTPD_400, "invalid_json", "expected {\"cmd\":\"010C\"}");
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s", cmd_item->valuestring);
    cJSON_Delete(json);

    if (toupper((unsigned char)cmd[0]) == 'A' && toupper((unsigned char)cmd[1]) == 'T') {
        return send_error_json(req, HTTPD_400, "not_supported",
                               "AT commands are not supported on the direct-CAN transport");
    }

    char norm[64];
    cmd_policy_normalize(cmd, norm, sizeof(norm));

    char resp[RESP_BUF_LEN];
    resp[0] = '\0';
    esp_err_t err = obd_poller_submit_raw(cmd, resp, sizeof(resp), 0);

    if (err == ESP_ERR_NOT_ALLOWED) {
        cmd_policy_config_t safety;
        if (profile_store_get_safety(&safety) != ESP_OK) {
            memset(&safety, 0, sizeof(safety));
        }
        cmd_policy_result_t why = cmd_policy_check(cmd, &safety);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "error", "blocked_by_policy");
        cJSON_AddStringToObject(root, "cmd", norm);
        cJSON_AddStringToObject(root, "reason", cmd_policy_result_str(why));
        return send_json(req, "403 Forbidden", root);
    }

    if (err != ESP_OK) {
        const char *http_status = HTTPD_500;
        if (err == ESP_ERR_INVALID_STATE) {
            http_status = "503 Service Unavailable";
        } else if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND) {
            http_status = "502 Bad Gateway";
        }
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "cmd", norm);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        return send_json(req, http_status, root);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "cmd", norm);
    cJSON_AddStringToObject(root, "raw", resp);
    try_decode(norm, resp, root);
    return send_ok_json(req, root);
}

/* ---- GET/PUT /api/profiles --------------------------------------------------- */

static esp_err_t api_profiles_get(httpd_req_t *req)
{
    char names[MAX_PROFILE_NAMES][32];
    int count = 0;
    esp_err_t err = profile_store_list(names, MAX_PROFILE_NAMES, &count);
    if (err != ESP_OK) {
        return send_error_json(req, HTTPD_500, "list_failed", esp_err_to_name(err));
    }

    obd_profile_t active;
    bool have_active = profile_store_get_active(&active) == ESP_OK;

    cJSON *root = cJSON_CreateObject();
    if (have_active) {
        cJSON_AddStringToObject(root, "active", active.name);
    } else {
        cJSON_AddNullToObject(root, "active");
    }

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; ++i) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    }
    cJSON_AddItemToObject(root, "profiles", arr);
    return send_ok_json(req, root);
}

static esp_err_t api_profiles_put(httpd_req_t *req)
{
    char *body = malloc(PROFILE_BODY_BUF_LEN);
    if (!body) {
        return send_error_json(req, HTTPD_500, "no_mem", NULL);
    }

    esp_err_t rc = read_body(req, body, PROFILE_BODY_BUF_LEN);
    if (rc != ESP_OK) {
        free(body);
        return send_error_json(req, HTTPD_400, "bad_request",
                                "body missing or too large (max 4KB)");
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return send_error_json(req, HTTPD_400, "invalid_json", NULL);
    }

    obd_profile_t profile;
    esp_err_t err = parse_profile_json(json, &profile);
    cJSON_Delete(json);
    if (err != ESP_OK) {
        return send_error_json(req, HTTPD_400, "invalid_profile", "missing/invalid \"name\"");
    }

    err = profile_store_upsert(&profile);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error_json(req, HTTPD_400, "policy_rejected",
                                "one or more cmd/init_at entries failed cmd_policy");
    }
    if (err != ESP_OK) {
        return send_error_json(req, HTTPD_500, "upsert_failed", esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "name", profile.name);
    return send_ok_json(req, root);
}

static esp_err_t api_profiles_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return api_profiles_get(req);
    }
    return api_profiles_put(req);
}

/* ---- POST /api/profiles/active ----------------------------------------------- */

static esp_err_t api_profiles_active_post(httpd_req_t *req)
{
    char body[REQ_BODY_BUF_LEN];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return send_error_json(req, HTTPD_400, "bad_request", "could not read body");
    }

    cJSON *json = cJSON_Parse(body);
    cJSON *name_item = json ? cJSON_GetObjectItem(json, "name") : NULL;
    if (!cJSON_IsString(name_item) || !name_item->valuestring || !name_item->valuestring[0]) {
        if (json) {
            cJSON_Delete(json);
        }
        return send_error_json(req, HTTPD_400, "invalid_json", "expected {\"name\":\"fleet_basic\"}");
    }

    char name[32];
    snprintf(name, sizeof(name), "%s", name_item->valuestring);
    cJSON_Delete(json);

    esp_err_t err = profile_store_set_active(name);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error_json(req, HTTPD_404, "not_found", "no such profile");
    }
    if (err != ESP_OK) {
        return send_error_json(req, HTTPD_400, "set_active_failed", esp_err_to_name(err));
    }

    err = obd_poller_reload_active_profile();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "poller reload failed: %s", esp_err_to_name(err));
    }

    /* No AT init sequence on direct CAN; can_boot_task enables polling once
     * the link supervisor confirms an ECU is answering. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "active", name);
    cJSON_AddBoolToObject(root, "can_ready", can_obd_is_ready());
    return send_ok_json(req, root);
}

/* ---- GET /api/telemetry ------------------------------------------------------- */

static esp_err_t api_telemetry_get(httpd_req_t *req)
{
    pid_ring_t ring_snapshot;
    telemetry_elm_event_t event_snapshot;
    bool have_event_snapshot;
    telemetry_error_msg_t error_snapshot;
    bool have_error_snapshot;
    telemetry_dtc_list_t dtc_snapshot;
    bool have_dtc_snapshot;

    if (s_cache_mutex != NULL) {
        xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
        ring_snapshot = s_pid_ring;
        event_snapshot = s_last_event;
        have_event_snapshot = s_have_event;
        error_snapshot = s_last_error;
        have_error_snapshot = s_have_error;
        dtc_snapshot = s_last_dtc;
        have_dtc_snapshot = s_have_dtc;
        xSemaphoreGive(s_cache_mutex);
    } else {
        memset(&ring_snapshot, 0, sizeof(ring_snapshot));
        have_event_snapshot = have_error_snapshot = have_dtc_snapshot = false;
    }

    uint64_t now = now_ms();
    cJSON *root = cJSON_CreateObject();
    cJSON *samples = cJSON_CreateArray();

    for (int i = 0; i < ring_snapshot.count; ++i) {
        int idx = (ring_snapshot.count < TELE_CACHE_CAP)
                      ? i
                      : (ring_snapshot.next + i) % TELE_CACHE_CAP;
        const telemetry_pid_sample_t *s = &ring_snapshot.items[idx];

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "cmd", s->cmd);
        cJSON_AddStringToObject(item, "name", s->name);
        cJSON_AddBoolToObject(item, "ok", s->ok);
        cJSON_AddNumberToObject(item, "value", s->value);
        cJSON_AddStringToObject(item, "unit", s->unit);
        cJSON_AddStringToObject(item, "raw_hex", s->raw_hex);
        cJSON_AddNumberToObject(item, "ts_ms", (double)s->ts_ms);
        cJSON_AddNumberToObject(item, "age_ms", (double)(now >= s->ts_ms ? now - s->ts_ms : 0));
        cJSON_AddItemToArray(samples, item);
    }
    cJSON_AddItemToObject(root, "samples", samples);

    if (have_event_snapshot) {
        static const char *kind_names[] = {"connected", "disconnected", "init_ok", "init_fail"};
        cJSON *ev = cJSON_CreateObject();
        int kind = (int)event_snapshot.kind;
        cJSON_AddStringToObject(ev, "kind",
                                (kind >= 0 && kind < 4) ? kind_names[kind] : "unknown");
        cJSON_AddNumberToObject(ev, "ts_ms", (double)event_snapshot.ts_ms);
        cJSON_AddItemToObject(root, "last_event", ev);
    } else {
        cJSON_AddNullToObject(root, "last_event");
    }

    if (have_error_snapshot) {
        cJSON *er = cJSON_CreateObject();
        cJSON_AddStringToObject(er, "cmd", error_snapshot.cmd);
        cJSON_AddStringToObject(er, "message", error_snapshot.message);
        cJSON_AddNumberToObject(er, "ts_ms", (double)error_snapshot.ts_ms);
        cJSON_AddItemToObject(root, "last_error", er);
    } else {
        cJSON_AddNullToObject(root, "last_error");
    }

    if (have_dtc_snapshot) {
        cJSON *dtc = cJSON_CreateObject();
        cJSON *codes = cJSON_CreateArray();
        for (int i = 0; i < dtc_snapshot.count; ++i) {
            cJSON_AddItemToArray(codes, cJSON_CreateString(dtc_snapshot.codes[i]));
        }
        cJSON_AddItemToObject(dtc, "codes", codes);
        cJSON_AddNumberToObject(dtc, "ts_ms", (double)dtc_snapshot.ts_ms);
        cJSON_AddItemToObject(root, "last_dtc", dtc);
    } else {
        cJSON_AddNullToObject(root, "last_dtc");
    }

    return send_ok_json(req, root);
}

/* ---- GET /api/metrics ---------------------------------------------------------- */

static esp_err_t api_metrics_get(httpd_req_t *req)
{
    char buf[METRICS_BUF_LEN];
    sys_runtime_metrics_snapshot_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* ---- GET /api/lte  and  POST /api/lte/reconnect ----------------------------- */

static void add_lte_json(cJSON *root, const net_lte_status_t *s)
{
    cJSON_AddBoolToObject(root, "enabled", s->enabled);
    cJSON_AddBoolToObject(root, "uart_ok", s->uart_ok);
    cJSON_AddBoolToObject(root, "sim_ready", s->sim_ready);
    cJSON_AddBoolToObject(root, "registered", s->registered);
    cJSON_AddBoolToObject(root, "attached", s->attached);
    cJSON_AddBoolToObject(root, "link_up", s->link_up);
    cJSON_AddBoolToObject(root, "ip_up", s->ip_up);
    cJSON_AddNumberToObject(root, "csq", s->csq);
    cJSON_AddNumberToObject(root, "rssi_dbm", s->rssi_dbm);
    cJSON_AddStringToObject(root, "operator", s->operator_name);
    cJSON_AddStringToObject(root, "apn", s->apn);
    cJSON_AddStringToObject(root, "ip", s->ip);
    cJSON_AddStringToObject(root, "module", s->ati);
    cJSON_AddStringToObject(root, "last_error", s->last_error);
}

static esp_err_t api_lte_get(httpd_req_t *req)
{
    (void)net_lte_refresh();
    net_lte_status_t s;
    net_lte_get_status(&s);
    cJSON *root = cJSON_CreateObject();
    add_lte_json(root, &s);
    return send_ok_json(req, root);
}

static esp_err_t api_lte_reconnect_post(httpd_req_t *req)
{
    esp_err_t err = net_lte_reconnect();
    net_lte_status_t s;
    net_lte_get_status(&s);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    add_lte_json(root, &s);
    return send_ok_json(req, root);
}

static esp_err_t api_lte_test_post(httpd_req_t *req)
{
    char report[768];
    esp_err_t err = net_lte_selftest(report, sizeof(report));
    net_lte_status_t s;
    net_lte_get_status(&s);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(root, "internet", s.link_up);
    cJSON_AddStringToObject(root, "report", report);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    add_lte_json(root, &s);
    return send_ok_json(req, root);
}

/* ---- Cloud uplink ---------------------------------------------------------- */

static void add_uplink_json(cJSON *root, const telemetry_uplink_status_t *st)
{
    cJSON_AddBoolToObject(root, "enabled", st->config.enabled);
    cJSON_AddNumberToObject(root, "interval_s", st->config.interval_s);
    cJSON_AddStringToObject(root, "device_id", st->config.device_id);
    cJSON_AddStringToObject(root, "node_id", st->config.node_id);
    cJSON_AddStringToObject(root, "schema_id", st->schema_id ? st->schema_id : "");
    cJSON_AddStringToObject(root, "url", st->url ? st->url : "");
    cJSON *last = cJSON_CreateObject();
    cJSON_AddBoolToObject(last, "ok", st->last.ok);
    cJSON_AddBoolToObject(last, "skipped", st->last.skipped);
    cJSON_AddNumberToObject(last, "http_status", st->last.http_status);
    cJSON_AddNumberToObject(last, "ts_ms", (double)st->last.ts_ms);
    uint64_t age = 0;
    if (st->last.ts_ms > 0) {
        uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
        age = (now >= st->last.ts_ms) ? (now - st->last.ts_ms) : 0;
    }
    cJSON_AddNumberToObject(last, "age_ms", (double)age);
    cJSON_AddStringToObject(last, "reason", st->last.reason);
    cJSON_AddStringToObject(last, "error", st->last.error);
    cJSON_AddItemToObject(root, "last", last);

    cJSON *q = cJSON_CreateObject();
    cJSON_AddBoolToObject(q, "sd_mounted", st->queue.sd_mounted);
    cJSON_AddNumberToObject(q, "depth", st->queue.queue_depth);
    cJSON_AddNumberToObject(q, "bytes", (double)st->queue.queue_bytes);
    cJSON_AddStringToObject(q, "drain_error", st->queue.drain_error);
    cJSON_AddItemToObject(root, "queue", q);
}

static esp_err_t api_uplink_get(httpd_req_t *req)
{
    telemetry_uplink_status_t st;
    esp_err_t err = telemetry_uplink_get_status(&st);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    cJSON *root = cJSON_CreateObject();
    add_uplink_json(root, &st);
    return send_ok_json(req, root);
}

static esp_err_t api_uplink_post(httpd_req_t *req)
{
    char body[320];
    esp_err_t err = read_body(req, body, sizeof(body));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
    }
    cJSON *json = cJSON_Parse(body);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    telemetry_uplink_config_t cfg;
    err = telemetry_uplink_get_config(&cfg);
    if (err != ESP_OK) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    cJSON *en = cJSON_GetObjectItem(json, "enabled");
    if (cJSON_IsBool(en)) {
        cfg.enabled = cJSON_IsTrue(en);
    }
    cJSON *iv = cJSON_GetObjectItem(json, "interval_s");
    if (cJSON_IsNumber(iv)) {
        int v = iv->valueint;
        if (v < 1) {
            v = 1;
        }
        if (v > 300) {
            v = 300;
        }
        cfg.interval_s = (uint16_t)v;
    }
    cJSON *did = cJSON_GetObjectItem(json, "device_id");
    if (cJSON_IsString(did) && did->valuestring) {
        snprintf(cfg.device_id, sizeof(cfg.device_id), "%s", did->valuestring);
    }
    cJSON *nid = cJSON_GetObjectItem(json, "node_id");
    if (cJSON_IsString(nid) && nid->valuestring) {
        snprintf(cfg.node_id, sizeof(cfg.node_id), "%s", nid->valuestring);
    }
    cJSON_Delete(json);

    err = telemetry_uplink_set_config(&cfg);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    }

    telemetry_uplink_status_t st;
    telemetry_uplink_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    add_uplink_json(root, &st);
    return send_ok_json(req, root);
}

static esp_err_t api_uplink_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return api_uplink_get(req);
    }
    return api_uplink_post(req);
}

static esp_err_t api_uplink_send_post(httpd_req_t *req)
{
    esp_err_t err = telemetry_uplink_send_now();
    telemetry_uplink_status_t st;
    telemetry_uplink_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    add_uplink_json(root, &st);
    return send_ok_json(req, root);
}

/* ---- diagnostics: DTC read / clear, VIN ------------------------------------- */

static esp_err_t run_obd_raw(const char *cmd, char *resp, size_t resp_len,
                             cJSON **err_root, const char **http_status)
{
    esp_err_t err = obd_poller_submit_raw(cmd, resp, resp_len, 0);
    if (err == ESP_ERR_NOT_ALLOWED) {
        cmd_policy_config_t safety;
        if (profile_store_get_safety(&safety) != ESP_OK) {
            memset(&safety, 0, sizeof(safety));
        }
        cmd_policy_result_t why = cmd_policy_check(cmd, &safety);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "error", "blocked_by_policy");
        cJSON_AddStringToObject(root, "cmd", cmd);
        cJSON_AddStringToObject(root, "reason", cmd_policy_result_str(why));
        *err_root = root;
        *http_status = "403 Forbidden";
        return err;
    }
    if (err != ESP_OK) {
        const char *st = HTTPD_500;
        if (err == ESP_ERR_INVALID_STATE) {
            st = "503 Service Unavailable";
        } else if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND) {
            st = "502 Bad Gateway";
        }
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "cmd", cmd);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        *err_root = root;
        *http_status = st;
        return err;
    }
    return ESP_OK;
}

static esp_err_t api_dtc_read_post(httpd_req_t *req)
{
    if (!can_obd_is_ready()) {
        return send_error_json(req, "503 Service Unavailable", "not_ready",
                               "CAN link not ready");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *stored = cJSON_CreateArray();
    cJSON *pending = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "stored", stored);
    cJSON_AddItemToObject(root, "pending", pending);

    const struct {
        const char *cmd;
        cJSON *arr;
    } queries[] = {{"03", stored}, {"07", pending}};

    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
        char resp[RESP_BUF_LEN];
        cJSON *err_root = NULL;
        const char *http_status = NULL;
        if (run_obd_raw(queries[i].cmd, resp, sizeof(resp), &err_root, &http_status) != ESP_OK) {
            cJSON_Delete(root);
            return send_json(req, http_status, err_root);
        }
        char dtcs[8][6];
        int n = obd_codec_parse_dtcs(resp, dtcs, 8);
        for (int j = 0; j < n; ++j) {
            cJSON_AddItemToArray(queries[i].arr, cJSON_CreateString(dtcs[j]));
        }
    }

    cJSON_AddBoolToObject(root, "ok", true);
    return send_ok_json(req, root);
}

static esp_err_t api_dtc_clear_post(httpd_req_t *req)
{
    if (!can_obd_is_ready()) {
        return send_error_json(req, "503 Service Unavailable", "not_ready",
                               "CAN link not ready");
    }

    char resp[RESP_BUF_LEN];
    cJSON *err_root = NULL;
    const char *http_status = NULL;
    /* Mode 04 is gated by cmd_policy; requires 'unsafe' enabled via /api/safety. */
    if (run_obd_raw("04", resp, sizeof(resp), &err_root, &http_status) != ESP_OK) {
        return send_json(req, http_status, err_root);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "raw", resp);
    cJSON_AddStringToObject(root, "note", "DTCs cleared; MIL may relearn on next drive cycle");
    return send_ok_json(req, root);
}

static esp_err_t api_vin_get(httpd_req_t *req)
{
    if (!can_obd_is_ready()) {
        return send_error_json(req, "503 Service Unavailable", "not_ready",
                               "CAN link not ready");
    }

    char resp[RESP_BUF_LEN];
    cJSON *err_root = NULL;
    const char *http_status = NULL;
    if (run_obd_raw("0902", resp, sizeof(resp), &err_root, &http_status) != ESP_OK) {
        return send_json(req, http_status, err_root);
    }

    cJSON *root = cJSON_CreateObject();
    char vin[18];
    if (obd_codec_parse_vin(resp, vin, sizeof(vin))) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "vin", vin);
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "raw", resp);
        cJSON_AddStringToObject(root, "note", "VIN not reported (Mode 09 PID 02 unsupported)");
    }
    return send_ok_json(req, root);
}

/* ---- GET/POST /api/safety --------------------------------------------------- */

static esp_err_t api_safety_get(httpd_req_t *req)
{
    cmd_policy_config_t safety;
    if (profile_store_get_safety(&safety) != ESP_OK) {
        memset(&safety, 0, sizeof(safety));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "allow_unsafe", safety.allow_unsafe);
    cJSON_AddStringToObject(root, "note",
                            "allow_unsafe permits Mode 04 (clear DTC). Mode 08 is never allowed.");
    return send_ok_json(req, root);
}

static esp_err_t api_safety_post(httpd_req_t *req)
{
    char body[REQ_BODY_BUF_LEN];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return send_error_json(req, HTTPD_400, "bad_request", "could not read body");
    }
    cJSON *json = cJSON_Parse(body);
    cJSON *item = json ? cJSON_GetObjectItem(json, "allow_unsafe") : NULL;
    if (!cJSON_IsBool(item)) {
        if (json) {
            cJSON_Delete(json);
        }
        return send_error_json(req, HTTPD_400, "invalid_json", "expected {\"allow_unsafe\":true}");
    }
    bool allow = cJSON_IsTrue(item);
    cJSON_Delete(json);

    esp_err_t err = profile_store_set_allow_unsafe(allow);
    if (err != ESP_OK) {
        return send_error_json(req, HTTPD_500, "persist_failed", esp_err_to_name(err));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "allow_unsafe", allow);
    return send_ok_json(req, root);
}

static esp_err_t api_safety_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return api_safety_get(req);
    }
    return api_safety_post(req);
}

/* ---- GET/POST /api/ota ------------------------------------------------------ */

static const char *fw_ota_state_name(fw_ota_state_t st)
{
    switch (st) {
    case FW_OTA_STATE_IDLE: return "idle";
    case FW_OTA_STATE_WRITING: return "writing";
    case FW_OTA_STATE_FAILED: return "failed";
    case FW_OTA_STATE_PENDING_REBOOT: return "pending_reboot";
    default: return "unknown";
    }
}

static esp_err_t api_ota_get(httpd_req_t *req)
{
    fw_ota_status_t st;
    if (fw_ota_get_status(&st) != ESP_OK) {
        return send_error_json(req, HTTPD_500, "status_failed", NULL);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", fw_ota_state_name(st.state));
    cJSON_AddStringToObject(root, "error", st.error);
    cJSON_AddStringToObject(root, "running_partition", st.running_partition);
    cJSON_AddStringToObject(root, "update_partition", st.update_partition);
    cJSON_AddStringToObject(root, "fw_version", st.fw_version);
    cJSON_AddNumberToObject(root, "expected_size", (double)st.expected_size);
    cJSON_AddNumberToObject(root, "bytes_written", (double)st.bytes_written);
    cJSON_AddBoolToObject(root, "pending_verify", st.pending_verify);
    return send_ok_json(req, root);
}

/*
 * LTE OTA endpoints (/api/ota/lte):
 *
 * - GET  /api/ota/lte
 *      Returns current LTE OTA config (manifest_url/channel/device_id/force),
 *      plus live phase/error and the latest seen manifest/applied versions.
 *
 * - POST /api/ota/lte
 *      Saves fields into NVS (see fw_ota_lte.c):
 *        manifest_url, channel, device_id, force
 *      Then returns GET /api/ota/lte.
 *
 * - POST /api/ota/lte/run
 *      Starts fw_ota_lte in the background (LTE GET manifest + download +
 *      flash inactive OTA slot + reboot).
 *
 * Auto-check (boot-time / periodic) uses force=false and the same URL stored
 * in NVS.
 */
#define OTA_RECV_CHUNK 4096

static esp_err_t api_ota_post(httpd_req_t *req)
{
    if (fw_ota_lte_is_busy() || fw_ota_is_busy()) {
        return send_error_json(req, "409 Conflict", "busy", "OTA already in progress");
    }

    char size_hdr[24] = {0};
    char sha_hdr[72] = {0};

    if (httpd_req_get_hdr_value_str(req, "X-Firmware-Size", size_hdr, sizeof(size_hdr)) != ESP_OK) {
        return send_error_json(req, HTTPD_400, "missing_header", "X-Firmware-Size required");
    }
    if (httpd_req_get_hdr_value_str(req, "X-Firmware-Sha256", sha_hdr, sizeof(sha_hdr)) != ESP_OK) {
        return send_error_json(req, HTTPD_400, "missing_header", "X-Firmware-Sha256 required");
    }

    /* Trim whitespace/newlines from sha header */
    size_t sha_len = strlen(sha_hdr);
    while (sha_len > 0 && isspace((unsigned char)sha_hdr[sha_len - 1])) {
        sha_hdr[--sha_len] = '\0';
    }

    char *end = NULL;
    unsigned long expected = strtoul(size_hdr, &end, 10);
    if (end == size_hdr || expected == 0) {
        return send_error_json(req, HTTPD_400, "bad_size", "X-Firmware-Size invalid");
    }
    if (req->content_len > 0 && (size_t)req->content_len != (size_t)expected) {
        return send_error_json(req, HTTPD_400, "size_mismatch",
                               "Content-Length does not match X-Firmware-Size");
    }

    esp_err_t err = fw_ota_begin((size_t)expected, sha_hdr);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error_json(req, "409 Conflict", "busy", "OTA already in progress");
    }
    if (err != ESP_OK) {
        fw_ota_status_t st;
        fw_ota_get_status(&st);
        return send_error_json(req, HTTPD_400, "begin_failed",
                               st.error[0] ? st.error : esp_err_to_name(err));
    }

    uint8_t *buf = malloc(OTA_RECV_CHUNK);
    if (!buf) {
        fw_ota_abort();
        return send_error_json(req, HTTPD_500, "no_mem", NULL);
    }

    size_t remaining = (size_t)expected;
    while (remaining > 0) {
        int to_read = (int)((remaining > OTA_RECV_CHUNK) ? OTA_RECV_CHUNK : remaining);
        int got = httpd_req_recv(req, (char *)buf, to_read);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            free(buf);
            fw_ota_abort();
            return send_error_json(req, HTTPD_500, "recv_failed", "connection closed early");
        }
        err = fw_ota_write(buf, (size_t)got);
        if (err != ESP_OK) {
            free(buf);
            fw_ota_status_t st;
            fw_ota_get_status(&st);
            return send_error_json(req, HTTPD_500, "write_failed",
                                   st.error[0] ? st.error : esp_err_to_name(err));
        }
        remaining -= (size_t)got;
    }
    free(buf);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "message", "verified; rebooting into new partition");
    esp_err_t send_err = send_ok_json(req, root);
    if (send_err != ESP_OK) {
        ESP_LOGW(TAG, "OTA response send failed: %s (still finalizing)", esp_err_to_name(send_err));
    }

    err = fw_ota_end_and_reboot();
    /* Only reached on failure */
    fw_ota_status_t st;
    fw_ota_get_status(&st);
    ESP_LOGE(TAG, "fw_ota_end_and_reboot failed: %s (%s)", esp_err_to_name(err), st.error);
    return ESP_FAIL;
}

static esp_err_t api_ota_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return api_ota_get(req);
    }
    return api_ota_post(req);
}

static const char *lte_phase_name(fw_ota_lte_phase_t p)
{
    switch (p) {
    case FW_OTA_LTE_IDLE: return "idle";
    case FW_OTA_LTE_CHECKING: return "checking";
    case FW_OTA_LTE_DOWNLOADING: return "downloading";
    case FW_OTA_LTE_NO_UPDATE: return "no_update";
    case FW_OTA_LTE_FAILED: return "failed";
    case FW_OTA_LTE_REBOOTING: return "rebooting";
    default: return "unknown";
    }
}

static esp_err_t api_ota_lte_get(httpd_req_t *req)
{
    fw_ota_lte_config_t cfg;
    fw_ota_lte_status_t st;
    fw_ota_status_t ost;
    fw_ota_lte_get_config(&cfg);
    fw_ota_lte_get_status(&st);
    fw_ota_get_status(&ost);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "manifest_url", cfg.manifest_url);
    cJSON_AddStringToObject(root, "channel", cfg.channel);
    cJSON_AddStringToObject(root, "device_id", cfg.device_id);
    cJSON_AddBoolToObject(root, "force", cfg.force);
    cJSON_AddStringToObject(root, "running_fw_version", ost.fw_version);
    cJSON_AddStringToObject(root, "running_partition", ost.running_partition);
    cJSON_AddStringToObject(root, "phase", lte_phase_name(st.phase));
    cJSON_AddStringToObject(root, "error", st.error);
    cJSON_AddStringToObject(root, "manifest_version", st.manifest_version);
    cJSON_AddStringToObject(root, "applied_version", st.applied_version);
    cJSON_AddNumberToObject(root, "http_status", st.http_status);
    cJSON_AddNumberToObject(root, "bytes_downloaded", (double)st.bytes_downloaded);
    cJSON_AddBoolToObject(root, "busy", fw_ota_lte_is_busy() || fw_ota_is_busy());
    return send_ok_json(req, root);
}

static esp_err_t api_ota_lte_post(httpd_req_t *req)
{
    char body[512];
    int r = httpd_req_recv(req, body, sizeof(body) - 1);
    if (r <= 0) {
        return send_error_json(req, HTTPD_400, "bad_request", "empty body");
    }
    body[r] = '\0';
    cJSON *json = cJSON_Parse(body);
    if (!json) {
        return send_error_json(req, HTTPD_400, "invalid_json", NULL);
    }
    fw_ota_lte_config_t cfg;
    fw_ota_lte_get_config(&cfg);
    cJSON *u = cJSON_GetObjectItem(json, "manifest_url");
    cJSON *ch = cJSON_GetObjectItem(json, "channel");
    cJSON *did = cJSON_GetObjectItem(json, "device_id");
    cJSON *force = cJSON_GetObjectItem(json, "force");
    if (cJSON_IsString(u)) {
        snprintf(cfg.manifest_url, sizeof(cfg.manifest_url), "%s", u->valuestring);
    }
    if (cJSON_IsString(ch)) {
        snprintf(cfg.channel, sizeof(cfg.channel), "%s", ch->valuestring);
    }
    if (cJSON_IsString(did)) {
        snprintf(cfg.device_id, sizeof(cfg.device_id), "%s", did->valuestring);
    }
    if (cJSON_IsBool(force)) {
        cfg.force = cJSON_IsTrue(force);
    }
    cJSON_Delete(json);
    esp_err_t err = fw_ota_lte_set_config(&cfg);
    if (err != ESP_OK) {
        return send_error_json(req, HTTPD_500, "save_failed", esp_err_to_name(err));
    }
    return api_ota_lte_get(req);
}

static esp_err_t api_ota_lte_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return api_ota_lte_get(req);
    }
    return api_ota_lte_post(req);
}

static esp_err_t api_ota_lte_run_post(httpd_req_t *req)
{
    esp_err_t err = fw_ota_lte_start_background();
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error_json(req, "409 Conflict", "busy", "LTE OTA already running");
    }
    if (err != ESP_OK) {
        return send_error_json(req, HTTPD_500, "start_failed", esp_err_to_name(err));
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "started");
    cJSON_AddStringToObject(root, "message", "LTE OTA running in background; poll /api/ota/lte");
    return send_ok_json(req, root);
}

/* ---- GET /api/health -------------------------------------------------------- */

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "other_wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "unknown";
    }
}

static esp_err_t api_health_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_s", (double)sys_runtime_metric_get("uptime_s"));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", (double)esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "reset_reason", reset_reason_str(esp_reset_reason()));

    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        cJSON_AddStringToObject(root, "fw_version", app->version);
        cJSON_AddStringToObject(root, "fw_project", app->project_name);
        cJSON_AddStringToObject(root, "idf_version", app->idf_ver);
        char built[40];
        snprintf(built, sizeof(built), "%s %s", app->date, app->time);
        cJSON_AddStringToObject(root, "built", built);
    }
    return send_ok_json(req, root);
}

/* ---- GET / ----------------------------------------------------------------------- */

static esp_err_t root_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, TRANSPORT_HTTP_INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ---- registration ------------------------------------------------------------------ */

esp_err_t http_api_register(httpd_handle_t server)
{
    esp_err_t err = telemetry_cache_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "telemetry_cache_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = api_status_get},
        {.uri = "/api/obd/cmd", .method = HTTP_POST, .handler = api_obd_cmd_post},
        {.uri = "/api/profiles", .method = HTTP_GET, .handler = api_profiles_handler},
        {.uri = "/api/profiles", .method = HTTP_PUT, .handler = api_profiles_handler},
        {.uri = "/api/profiles/active", .method = HTTP_POST, .handler = api_profiles_active_post},
        {.uri = "/api/telemetry", .method = HTTP_GET, .handler = api_telemetry_get},
        {.uri = "/api/metrics", .method = HTTP_GET, .handler = api_metrics_get},
        {.uri = "/api/lte", .method = HTTP_GET, .handler = api_lte_get},
        {.uri = "/api/lte/reconnect", .method = HTTP_POST, .handler = api_lte_reconnect_post},
        {.uri = "/api/lte/test", .method = HTTP_POST, .handler = api_lte_test_post},
        {.uri = "/api/uplink", .method = HTTP_GET, .handler = api_uplink_handler},
        {.uri = "/api/uplink", .method = HTTP_POST, .handler = api_uplink_handler},
        {.uri = "/api/uplink/send", .method = HTTP_POST, .handler = api_uplink_send_post},
        {.uri = "/api/dtc/read", .method = HTTP_POST, .handler = api_dtc_read_post},
        {.uri = "/api/dtc/clear", .method = HTTP_POST, .handler = api_dtc_clear_post},
        {.uri = "/api/vin", .method = HTTP_GET, .handler = api_vin_get},
        {.uri = "/api/safety", .method = HTTP_GET, .handler = api_safety_handler},
        {.uri = "/api/safety", .method = HTTP_POST, .handler = api_safety_handler},
        {.uri = "/api/health", .method = HTTP_GET, .handler = api_health_get},
        {.uri = "/api/ota", .method = HTTP_GET, .handler = api_ota_handler},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = api_ota_handler},
        {.uri = "/api/ota/lte", .method = HTTP_GET, .handler = api_ota_lte_handler},
        {.uri = "/api/ota/lte", .method = HTTP_POST, .handler = api_ota_lte_handler},
        {.uri = "/api/ota/lte/run", .method = HTTP_POST, .handler = api_ota_lte_run_post},
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to register %s: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "registered %d URI handlers", (int)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
