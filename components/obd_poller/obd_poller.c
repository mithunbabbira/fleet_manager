#include "obd_poller.h"

#include "can_obd.h"
#include "cmd_policy.h"
#include "obd_codec.h"
#include "profile_store.h"
#include "sdkconfig.h"
#include "sys_runtime.h"
#include "telemetry_bus.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <limits.h>
#include <string.h>

static const char *TAG = "obd_poller";

#define POLLER_TASK_STACK   4096
#define POLLER_TASK_PRIO    5
#define RAW_QUEUE_DEPTH     2
#define MAX_PROFILE_ITEMS   32
#define POLL_RESP_BUF_LEN   256
#define RAW_CMD_BUF_LEN     64
#define RAW_SUBMIT_WAIT_MS  5000

typedef struct {
    char cmd[RAW_CMD_BUF_LEN];
    char *resp;
    size_t resp_len;
    uint32_t timeout_ms;
    esp_err_t *result_out;
    SemaphoreHandle_t done;
} raw_request_t;

static TaskHandle_t s_task_handle;
static QueueHandle_t s_raw_queue;
static SemaphoreHandle_t s_profile_mutex;
static obd_profile_t s_profile;
static bool s_profile_loaded;
static uint64_t s_last_fire_ms[MAX_PROFILE_ITEMS];
static volatile bool s_stop_requested;
static bool s_running;
static volatile bool s_poll_enabled;
static uint32_t s_bus_fail_backoff_ms;
static uint64_t s_bus_fail_until_ms;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void publish_cmd_blocked(const char *cmd, cmd_policy_result_t policy)
{
    telemetry_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = TELEMETRY_ERROR;
    msg.error.code = TELEMETRY_ERROR_CMD_BLOCKED;
    if (cmd) {
        snprintf(msg.error.cmd, sizeof(msg.error.cmd), "%s", cmd);
    }
    snprintf(msg.error.message, sizeof(msg.error.message), "%s",
             cmd_policy_result_str(policy));
    msg.error.ts_ms = now_ms();
    telemetry_publish(&msg);
}

static void publish_elm_error(const char *cmd, esp_err_t err)
{
    telemetry_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = TELEMETRY_ERROR;
    if (cmd) {
        snprintf(msg.error.cmd, sizeof(msg.error.cmd), "%s", cmd);
    }
    msg.error.ts_ms = now_ms();

    switch (err) {
    case ESP_ERR_TIMEOUT:
        msg.error.code = TELEMETRY_ERROR_ELM_TIMEOUT;
        snprintf(msg.error.message, sizeof(msg.error.message), "elm timeout");
        break;
    case ESP_ERR_NOT_FOUND:
        msg.error.code = TELEMETRY_ERROR_ELM_NO_DATA;
        snprintf(msg.error.message, sizeof(msg.error.message), "no data");
        break;
    default:
        msg.error.code = TELEMETRY_ERROR_ELM_BUS_ERROR;
        snprintf(msg.error.message, sizeof(msg.error.message), "%s",
                 esp_err_to_name(err));
        break;
    }

    telemetry_publish(&msg);
}

static void publish_pid_sample(const char *cmd, const char *decode_key,
                               const char *resp, bool decode_ok)
{
    telemetry_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = TELEMETRY_PID_SAMPLE;
    msg.pid_sample.ts_ms = now_ms();

    if (cmd) {
        snprintf(msg.pid_sample.cmd, sizeof(msg.pid_sample.cmd), "%s", cmd);
    }

    if (decode_ok) {
        obd_decoded_t decoded;
        if (obd_codec_decode_named(decode_key, resp, &decoded) && decoded.ok) {
            msg.pid_sample.ok = true;
            snprintf(msg.pid_sample.name, sizeof(msg.pid_sample.name), "%s",
                     decoded.name ? decoded.name : decode_key);
            snprintf(msg.pid_sample.unit, sizeof(msg.pid_sample.unit), "%s",
                     decoded.unit ? decoded.unit : "");
            snprintf(msg.pid_sample.raw_hex, sizeof(msg.pid_sample.raw_hex), "%s",
                     decoded.raw_hex);
            msg.pid_sample.value = decoded.value;
            telemetry_publish(&msg);
            return;
        }
    }

    msg.pid_sample.ok = false;
    snprintf(msg.pid_sample.name, sizeof(msg.pid_sample.name), "%s",
             decode_key ? decode_key : "");
    telemetry_publish(&msg);
}

static void publish_vin_sample(const char *cmd, const char *resp, bool parse_ok)
{
    telemetry_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = TELEMETRY_PID_SAMPLE;
    msg.pid_sample.ts_ms = now_ms();

    if (cmd) {
        snprintf(msg.pid_sample.cmd, sizeof(msg.pid_sample.cmd), "%s", cmd);
    }
    snprintf(msg.pid_sample.name, sizeof(msg.pid_sample.name), "vin");

    if (parse_ok) {
        char vin[18];
        if (obd_codec_parse_vin(resp, vin, sizeof(vin))) {
            msg.pid_sample.ok = true;
            snprintf(msg.pid_sample.raw_hex, sizeof(msg.pid_sample.raw_hex), "%s", vin);
            telemetry_publish(&msg);
            return;
        }
    }

    msg.pid_sample.ok = false;
    telemetry_publish(&msg);
}

static void publish_dtc_list(const char *resp)
{
    telemetry_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = TELEMETRY_DTC_LIST;
    msg.dtc_list.ts_ms = now_ms();
    msg.dtc_list.source_mode = 0x03;
    msg.dtc_list.count = obd_codec_parse_dtcs(resp, msg.dtc_list.codes,
                                                (int)(sizeof(msg.dtc_list.codes) /
                                                      sizeof(msg.dtc_list.codes[0])));
    telemetry_publish(&msg);
}

static esp_err_t run_policy_and_transact(const char *cmd, char *resp, size_t resp_len,
                                         uint32_t timeout_ms)
{
    cmd_policy_config_t safety;
    if (profile_store_get_safety(&safety) != ESP_OK) {
        memset(&safety, 0, sizeof(safety));
    }

    cmd_policy_result_t policy = cmd_policy_check(cmd, &safety);
    if (policy != CMD_POLICY_ALLOW) {
        publish_cmd_blocked(cmd, policy);
        sys_runtime_metric_inc("blocked_cmds");
        return ESP_ERR_NOT_ALLOWED;
    }

    if (timeout_ms == 0) {
        timeout_ms = CONFIG_ELM_CMD_TIMEOUT_MS;
    }

    esp_err_t err = can_obd_transact(cmd, resp, resp_len, timeout_ms);
    if (err == ESP_OK) {
        sys_runtime_metric_inc("cmds_ok");
    } else {
        sys_runtime_metric_inc("cmds_fail");
        publish_elm_error(cmd, err);
    }

    return err;
}

static void handle_poll_response(const profile_item_t *item, const char *resp)
{
    if (!item || !resp) {
        return;
    }

    if (strcmp(item->decode, "dtc") == 0) {
        publish_dtc_list(resp);
        return;
    }

    if (strcmp(item->decode, "vin") == 0) {
        publish_vin_sample(item->cmd, resp, true);
        return;
    }

    publish_pid_sample(item->cmd, item->decode, resp, true);
}

static int pick_due_item_locked(uint64_t now)
{
    int best_idx = -1;
    uint64_t best_overdue = 0;

    for (int i = 0; i < s_profile.item_count && i < MAX_PROFILE_ITEMS; ++i) {
        uint64_t elapsed = now - s_last_fire_ms[i];
        if (elapsed >= s_profile.items[i].interval_ms) {
            if (best_idx < 0 || elapsed > best_overdue) {
                best_idx = i;
                best_overdue = elapsed;
            }
        }
    }

    return best_idx;
}

static uint32_t compute_wait_ms_locked(uint64_t now)
{
    if (!s_profile_loaded || s_profile.item_count <= 0) {
        return 500;
    }

    uint64_t soonest = UINT64_MAX;
    for (int i = 0; i < s_profile.item_count && i < MAX_PROFILE_ITEMS; ++i) {
        uint64_t elapsed = now - s_last_fire_ms[i];
        if (elapsed >= s_profile.items[i].interval_ms) {
            return 0;
        }
        uint64_t remaining = s_profile.items[i].interval_ms - elapsed;
        if (remaining < soonest) {
            soonest = remaining;
        }
    }

    if (soonest == UINT64_MAX) {
        return 500;
    }
    if (soonest > 500) {
        return 500;
    }
    return (uint32_t)soonest;
}

static void drain_raw_queue_on_stop(void)
{
    raw_request_t raw;

    while (xQueueReceive(s_raw_queue, &raw, 0) == pdTRUE) {
        if (raw.result_out != NULL) {
            *raw.result_out = ESP_ERR_INVALID_STATE;
        }
        if (raw.done != NULL) {
            xSemaphoreGive(raw.done);
        }
    }
}

static void handle_raw_request(const raw_request_t *raw)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;

    if (raw != NULL && raw->resp != NULL && raw->resp_len > 0) {
        result = run_policy_and_transact(raw->cmd, raw->resp, raw->resp_len,
                                         raw->timeout_ms);
    }

    if (raw != NULL) {
        if (raw->result_out != NULL) {
            *raw->result_out = result;
        }
        if (raw->done != NULL) {
            xSemaphoreGive(raw->done);
        }
    }
}

static void poller_task(void *arg)
{
    (void)arg;
    char resp[POLL_RESP_BUF_LEN];

    ESP_LOGI(TAG, "poller task started");

    while (!s_stop_requested) {
        raw_request_t raw;
        if (xQueueReceive(s_raw_queue, &raw, 0) == pdTRUE) {
            handle_raw_request(&raw);
            continue;
        }

        if (!can_obd_is_ready()) {
            if (xQueueReceive(s_raw_queue, &raw, pdMS_TO_TICKS(500)) == pdTRUE) {
                handle_raw_request(&raw);
            }
            continue;
        }

        /* Raw console/HTTP commands always run; profile polling is gated. */
        if (!s_poll_enabled) {
            if (xQueueReceive(s_raw_queue, &raw, pdMS_TO_TICKS(200)) == pdTRUE) {
                handle_raw_request(&raw);
            }
            continue;
        }

        uint64_t now = now_ms();
        if (s_bus_fail_until_ms != 0 && now < s_bus_fail_until_ms) {
            uint32_t wait_ms = (uint32_t)(s_bus_fail_until_ms - now);
            if (wait_ms > 500) {
                wait_ms = 500;
            }
            if (xQueueReceive(s_raw_queue, &raw, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
                handle_raw_request(&raw);
            }
            continue;
        }

        int item_idx = -1;
        profile_item_t item;

        if (xSemaphoreTake(s_profile_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_profile_loaded) {
                item_idx = pick_due_item_locked(now);
                if (item_idx >= 0) {
                    item = s_profile.items[item_idx];
                }
            }
            xSemaphoreGive(s_profile_mutex);
        }

        if (item_idx < 0) {
            uint32_t wait_ms = 500;
            if (xSemaphoreTake(s_profile_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                wait_ms = compute_wait_ms_locked(now);
                xSemaphoreGive(s_profile_mutex);
            }

            if (xQueueReceive(s_raw_queue, &raw, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
                handle_raw_request(&raw);
            }
            continue;
        }

        /* ELM-era AT items (e.g. ATRV voltage) have no direct-CAN equivalent. */
        if ((item.cmd[0] == 'A' || item.cmd[0] == 'a') &&
            (item.cmd[1] == 'T' || item.cmd[1] == 't')) {
            static bool s_warned_at;
            if (!s_warned_at) {
                ESP_LOGW(TAG, "skipping AT profile items (no ELM327 on CAN transport)");
                s_warned_at = true;
            }
            if (xSemaphoreTake(s_profile_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_last_fire_ms[item_idx] = now_ms();
                xSemaphoreGive(s_profile_mutex);
            }
            continue;
        }

        esp_err_t err = run_policy_and_transact(item.cmd, resp, sizeof(resp),
                                                CONFIG_ELM_CMD_TIMEOUT_MS);
        if (err == ESP_OK) {
            handle_poll_response(&item, resp);
            s_bus_fail_backoff_ms = 0;
            s_bus_fail_until_ms = 0;
        } else if (err == ESP_FAIL || err == ESP_ERR_TIMEOUT) {
            /* Back off hard on bus/protocol failures so we don't spam ATSP search. */
            if (s_bus_fail_backoff_ms == 0) {
                s_bus_fail_backoff_ms = 2000;
            } else if (s_bus_fail_backoff_ms < 15000) {
                s_bus_fail_backoff_ms *= 2;
                if (s_bus_fail_backoff_ms > 15000) {
                    s_bus_fail_backoff_ms = 15000;
                }
            }
            s_bus_fail_until_ms = now_ms() + s_bus_fail_backoff_ms;
            ESP_LOGW(TAG, "bus error on %s (%s); backoff %lu ms", item.cmd,
                     esp_err_to_name(err), (unsigned long)s_bus_fail_backoff_ms);
        }

        if (xSemaphoreTake(s_profile_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (item_idx >= 0 && item_idx < MAX_PROFILE_ITEMS) {
                s_last_fire_ms[item_idx] = now_ms();
            }
            xSemaphoreGive(s_profile_mutex);
        }
    }

    drain_raw_queue_on_stop();
    s_task_handle = NULL;
    ESP_LOGI(TAG, "poller task stopped");
    vTaskDelete(NULL);
}

esp_err_t obd_poller_reload_active_profile(void)
{
    obd_profile_t profile;
    esp_err_t err = profile_store_get_active(&profile);
    if (err != ESP_OK) {
        return err;
    }

    if (s_profile_mutex == NULL) {
        s_profile_mutex = xSemaphoreCreateMutex();
        if (s_profile_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_profile_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    s_profile = profile;
    s_profile_loaded = true;
    memset(s_last_fire_ms, 0, sizeof(s_last_fire_ms));
    xSemaphoreGive(s_profile_mutex);

    ESP_LOGI(TAG, "loaded profile \"%s\" (%d items)", profile.name, profile.item_count);
    return ESP_OK;
}

esp_err_t obd_poller_start(void)
{
    if (s_running && s_task_handle != NULL) {
        return ESP_OK;
    }

    if (s_raw_queue == NULL) {
        s_raw_queue = xQueueCreate(RAW_QUEUE_DEPTH, sizeof(raw_request_t));
        if (s_raw_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = obd_poller_reload_active_profile();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "profile load failed: %s", esp_err_to_name(err));
    }

    s_stop_requested = false;
    s_poll_enabled = false; /* enabled after ELM init sequence succeeds */
    s_bus_fail_backoff_ms = 0;
    s_bus_fail_until_ms = 0;
    BaseType_t created = xTaskCreate(poller_task, "obd_poller", POLLER_TASK_STACK, NULL,
                                     POLLER_TASK_PRIO, &s_task_handle);
    if (created != pdPASS) {
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    return ESP_OK;
}

void obd_poller_set_enabled(bool enabled)
{
    s_poll_enabled = enabled;
    if (enabled) {
        s_bus_fail_backoff_ms = 0;
        s_bus_fail_until_ms = 0;
    }
    ESP_LOGI(TAG, "profile polling %s", enabled ? "enabled" : "paused");
}

bool obd_poller_is_enabled(void)
{
    return s_poll_enabled;
}

esp_err_t obd_poller_stop(void)
{
    if (!s_running || s_task_handle == NULL) {
        return ESP_OK;
    }

    s_stop_requested = true;
    s_running = false;

    for (int i = 0; i < 200; ++i) {
        if (s_task_handle == NULL) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_task_handle != NULL) {
        drain_raw_queue_on_stop();
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }

    s_stop_requested = false;
    return ESP_OK;
}

esp_err_t obd_poller_submit_raw(const char *cmd, char *resp, size_t resp_len,
                                uint32_t timeout_ms)
{
    if (!s_running || s_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cmd == NULL || resp == NULL || resp_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_raw_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    raw_request_t req;
    esp_err_t result = ESP_FAIL;

    memset(&req, 0, sizeof(req));
    snprintf(req.cmd, sizeof(req.cmd), "%s", cmd);
    req.resp = resp;
    req.resp_len = resp_len;
    req.timeout_ms = timeout_ms;
    req.result_out = &result;
    req.done = xSemaphoreCreateBinary();
    if (req.done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xQueueSend(s_raw_queue, &req, pdMS_TO_TICKS(RAW_SUBMIT_WAIT_MS)) != pdTRUE) {
        vSemaphoreDelete(req.done);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(req.done, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(req.done);
        return ESP_FAIL;
    }

    vSemaphoreDelete(req.done);
    return result;
}
