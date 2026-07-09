#include "http_api.h"

#include "static_index.html.h"

#include "ble_elm.h"
#include "cmd_policy.h"
#include "elm327_client.h"
#include "obd_codec.h"
#include "obd_poller.h"
#include "profile_store.h"
#include "sys_runtime.h"
#include "telemetry_bus.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "http_api";

#define REQ_BODY_BUF_LEN     512
#define PROFILE_BODY_BUF_LEN 4096
#define RESP_BUF_LEN         256
#define METRICS_BUF_LEN      768
#define MAX_SCAN_DEVICES     16
#define MAX_PROFILE_NAMES    16

#define BLE_READY_WAIT_MS 8000
#define BLE_READY_POLL_MS 200

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

static void format_addr(const uint8_t addr[6], char *out, size_t len)
{
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1], addr[2], addr[3],
             addr[4], addr[5]);
}

static bool parse_addr_str(const char *s, uint8_t addr[6])
{
    if (!s) {
        return false;
    }
    unsigned vals[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &vals[0], &vals[1], &vals[2], &vals[3], &vals[4],
               &vals[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        addr[i] = (uint8_t)vals[i];
    }
    return true;
}

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
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ble_connected", ble_elm_is_connected());
    cJSON_AddBoolToObject(root, "elm_ready", elm327_client_is_ready());

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

/* ---- POST /api/ble/scan --------------------------------------------------- */

static void scan_task(void *arg)
{
    (void)arg;
    ble_elm_start_scan(0); /* 0 => Kconfig default duration */
    vTaskDelete(NULL);
}

static esp_err_t api_ble_scan_post(httpd_req_t *req)
{
    BaseType_t created = xTaskCreate(scan_task, "http_ble_scan", 4096, NULL, 4, NULL);
    if (created != pdPASS) {
        return send_error_json(req, HTTPD_500, "scan_start_failed", NULL);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "status", "scanning");
    return send_ok_json(req, root);
}

/* ---- GET /api/ble/devices -------------------------------------------------- */

static esp_err_t api_ble_devices_get(httpd_req_t *req)
{
    ble_elm_device_t devices[MAX_SCAN_DEVICES];
    int count = 0;
    esp_err_t err = ble_elm_get_scan_results(devices, MAX_SCAN_DEVICES, &count);
    if (err != ESP_OK) {
        return send_error_json(req, HTTPD_500, "scan_results_failed", esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; ++i) {
        char addr_str[18];
        format_addr(devices[i].addr, addr_str, sizeof(addr_str));

        cJSON *dev = cJSON_CreateObject();
        cJSON_AddNumberToObject(dev, "index", i);
        cJSON_AddStringToObject(dev, "name", devices[i].name);
        cJSON_AddStringToObject(dev, "addr", addr_str);
        cJSON_AddNumberToObject(dev, "rssi", devices[i].rssi);
        cJSON_AddItemToArray(arr, dev);
    }
    cJSON_AddItemToObject(root, "devices", arr);
    return send_ok_json(req, root);
}

/* ---- POST /api/ble/select --------------------------------------------------- */

static esp_err_t api_ble_select_post(httpd_req_t *req)
{
    char body[REQ_BODY_BUF_LEN];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return send_error_json(req, HTTPD_400, "bad_request", "could not read body");
    }

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        return send_error_json(req, HTTPD_400, "invalid_json", NULL);
    }

    uint8_t addr[6];
    char name[32] = {0};
    bool have_addr = false;

    cJSON *addr_item = cJSON_GetObjectItem(json, "addr");
    cJSON *index_item = cJSON_GetObjectItem(json, "index");

    if (cJSON_IsString(addr_item) && addr_item->valuestring) {
        have_addr = parse_addr_str(addr_item->valuestring, addr);
    } else if (cJSON_IsNumber(index_item)) {
        int idx = index_item->valueint;
        ble_elm_device_t devices[MAX_SCAN_DEVICES];
        int count = 0;
        if (ble_elm_get_scan_results(devices, MAX_SCAN_DEVICES, &count) == ESP_OK && idx >= 0 &&
            idx < count) {
            memcpy(addr, devices[idx].addr, sizeof(addr));
            snprintf(name, sizeof(name), "%s", devices[idx].name);
            have_addr = true;
        }
    }
    cJSON_Delete(json);

    if (!have_addr) {
        return send_error_json(req, HTTPD_400, "invalid_target",
                                "provide {\"index\":N} from /api/ble/devices or {\"addr\":\"AA:BB:CC:DD:EE:FF\"}");
    }

    ble_bond_t bond;
    memset(&bond, 0, sizeof(bond));
    memcpy(bond.addr, addr, sizeof(bond.addr));
    bond.addr_set = true;
    if (name[0]) {
        snprintf(bond.name, sizeof(bond.name), "%s", name);
    }
    profile_store_set_bond(&bond);

    esp_err_t err = ble_elm_connect_addr(addr);
    if (err != ESP_OK) {
        return send_error_json(req, "502 Bad Gateway", "connect_failed", esp_err_to_name(err));
    }

    elm327_client_set_transport(ble_elm_get_transport());

    int waited_ms = 0;
    while (!elm327_client_is_ready() && waited_ms < BLE_READY_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(BLE_READY_POLL_MS));
        waited_ms += BLE_READY_POLL_MS;
    }

    bool ready = elm327_client_is_ready();
    bool connected = ble_elm_is_connected();
    bool init_ok = false;

    if (ready) {
        obd_profile_t profile;
        if (profile_store_get_active(&profile) == ESP_OK && profile.init_at_count > 0) {
            init_ok = elm327_client_run_init_sequence(profile.init_at, profile.init_at_count) ==
                      ESP_OK;
        } else {
            init_ok = true;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", connected);
    cJSON_AddBoolToObject(root, "ready", ready);
    cJSON_AddBoolToObject(root, "init_ok", init_ok);
    return send_ok_json(req, root);
}

/* ---- POST /api/elm/cmd ------------------------------------------------------- */

static esp_err_t api_elm_cmd_post(httpd_req_t *req)
{
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

    bool init_ok = false;
    if (elm327_client_is_ready()) {
        obd_profile_t profile;
        if (profile_store_get_active(&profile) == ESP_OK && profile.init_at_count > 0) {
            init_ok =
                elm327_client_run_init_sequence(profile.init_at, profile.init_at_count) == ESP_OK;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "active", name);
    cJSON_AddBoolToObject(root, "init_ok", init_ok);
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

/* ---- GET / ----------------------------------------------------------------------- */

static esp_err_t root_get(httpd_req_t *req)
{
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
        {.uri = "/api/ble/scan", .method = HTTP_POST, .handler = api_ble_scan_post},
        {.uri = "/api/ble/devices", .method = HTTP_GET, .handler = api_ble_devices_get},
        {.uri = "/api/ble/select", .method = HTTP_POST, .handler = api_ble_select_post},
        {.uri = "/api/elm/cmd", .method = HTTP_POST, .handler = api_elm_cmd_post},
        {.uri = "/api/profiles", .method = HTTP_GET, .handler = api_profiles_handler},
        {.uri = "/api/profiles", .method = HTTP_PUT, .handler = api_profiles_handler},
        {.uri = "/api/profiles/active", .method = HTTP_POST, .handler = api_profiles_active_post},
        {.uri = "/api/telemetry", .method = HTTP_GET, .handler = api_telemetry_get},
        {.uri = "/api/metrics", .method = HTTP_GET, .handler = api_metrics_get},
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
