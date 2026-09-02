#include "sys_runtime.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef CONFIG_ESP_TASK_WDT_TIMEOUT_S
#define CONFIG_ESP_TASK_WDT_TIMEOUT_S 10
#endif

static const char *TAG = "sys_runtime";

#define SYS_RUNTIME_MAX_METRICS 16
#define SYS_RUNTIME_HEARTBEAT_STACK 3072
#define SYS_RUNTIME_HEARTBEAT_PRIO (tskIDLE_PRIORITY + 1)
#define SYS_RUNTIME_LOG_INTERVAL_S 60

typedef struct {
    char name[24];
    uint64_t value;
    bool used;
} sys_runtime_metric_t;

static sys_runtime_metric_t s_metrics[SYS_RUNTIME_MAX_METRICS];
static portMUX_TYPE s_metrics_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief Locate an existing metric by name.
 * @note Callers must hold s_metrics_lock.
 * @return Pointer into s_metrics, or NULL if not found.
 */
static sys_runtime_metric_t *find_metric_locked(const char *name)
{
    for (int i = 0; i < SYS_RUNTIME_MAX_METRICS; ++i) {
        if (s_metrics[i].used && strcmp(s_metrics[i].name, name) == 0) {
            return &s_metrics[i];
        }
    }
    return NULL;
}

/**
 * @brief Find a metric or allocate a free slot (value starts at 0).
 * @note Callers must hold s_metrics_lock.
 * @return Metric pointer, or NULL if the table is full.
 */
static sys_runtime_metric_t *find_or_create_metric_locked(const char *name)
{
    sys_runtime_metric_t *m = find_metric_locked(name);
    if (m) {
        return m;
    }
    for (int i = 0; i < SYS_RUNTIME_MAX_METRICS; ++i) {
        if (!s_metrics[i].used) {
            s_metrics[i].used = true;
            strncpy(s_metrics[i].name, name, sizeof(s_metrics[i].name) - 1);
            s_metrics[i].name[sizeof(s_metrics[i].name) - 1] = '\0';
            s_metrics[i].value = 0;
            return &s_metrics[i];
        }
    }
    return NULL;
}

/**
 * @brief Pre-create the fixed set of fleet counters so snapshots always include them.
 */
static void register_known_metrics(void)
{
    static const char *known[] = {
        "cmds_ok", "cmds_fail", "ble_reconnects",
        "blocked_cmds", "telemetry_drops", "uptime_s",
    };
    portENTER_CRITICAL(&s_metrics_lock);
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        find_or_create_metric_locked(known[i]);
    }
    portEXIT_CRITICAL(&s_metrics_lock);
}

/**
 * @brief Low-prio loop: pet TWDT every 1s and publish uptime_s.
 * @note Survives TWDT add failure with a warning; never returns.
 */
static void heartbeat_task(void *arg)
{
    (void)arg;

    esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_task_wdt_add failed: %s", esp_err_to_name(err));
    }

    const int64_t start_us = esp_timer_get_time();
    uint32_t last_logged_s = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_task_wdt_reset();

        uint32_t uptime_s = (uint32_t)((esp_timer_get_time() - start_us) / 1000000);

        portENTER_CRITICAL(&s_metrics_lock);
        sys_runtime_metric_t *m = find_or_create_metric_locked("uptime_s");
        if (m) {
            m->value = uptime_s;
        }
        portEXIT_CRITICAL(&s_metrics_lock);

        if (uptime_s - last_logged_s >= SYS_RUNTIME_LOG_INTERVAL_S) {
            ESP_LOGD(TAG, "uptime: %" PRIu32 "s", uptime_s);
            last_logged_s = uptime_s;
        }
    }
}

/**
 * @brief Init TWDT (or accept already-running), seed metrics, spawn heartbeat.
 * @return ESP_OK on success; ESP_ERR_NO_MEM if task create fails; else TWDT error.
 */
esp_err_t sys_runtime_init(void)
{
    register_known_metrics();

    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic = false,
    };
    esp_err_t err = esp_task_wdt_init(&wdt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE just means the TWDT was already started by
         * the system startup code (CONFIG_ESP_TASK_WDT_INIT); anything else
         * is a real failure. */
        ESP_LOGE(TAG, "esp_task_wdt_init failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(heartbeat_task, "sys_heartbeat",
                                 SYS_RUNTIME_HEARTBEAT_STACK, NULL,
                                 SYS_RUNTIME_HEARTBEAT_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create heartbeat task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief Atomically bump a named counter (create-on-first-use).
 * @note No-op if @p name is NULL or metric table is full.
 */
void sys_runtime_metric_inc(const char *name)
{
    if (!name) {
        return;
    }
    portENTER_CRITICAL(&s_metrics_lock);
    sys_runtime_metric_t *m = find_or_create_metric_locked(name);
    if (m) {
        m->value++;
    }
    portEXIT_CRITICAL(&s_metrics_lock);
}

/**
 * @brief Atomically read a named counter.
 * @return Value, or 0 if @p name is NULL / unknown.
 */
uint64_t sys_runtime_metric_get(const char *name)
{
    if (!name) {
        return 0;
    }
    uint64_t value = 0;
    portENTER_CRITICAL(&s_metrics_lock);
    sys_runtime_metric_t *m = find_metric_locked(name);
    if (m) {
        value = m->value;
    }
    portEXIT_CRITICAL(&s_metrics_lock);
    return value;
}

/**
 * @brief Snapshot metrics under lock, then format `{ "k":v,... }` without holding it.
 * @note Truncates and still NUL-terminates if @p len is insufficient.
 */
void sys_runtime_metrics_snapshot_json(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    sys_runtime_metric_t snapshot[SYS_RUNTIME_MAX_METRICS];
    portENTER_CRITICAL(&s_metrics_lock);
    memcpy(snapshot, s_metrics, sizeof(snapshot));
    portEXIT_CRITICAL(&s_metrics_lock);

    size_t pos = 0;
    int n = snprintf(buf, len, "{");
    if (n > 0) {
        pos += (size_t)n;
    }

    bool first = true;
    for (int i = 0; i < SYS_RUNTIME_MAX_METRICS && pos < len; ++i) {
        if (!snapshot[i].used) {
            continue;
        }
        n = snprintf(buf + pos, len - pos, "%s\"%s\":%llu",
                     first ? "" : ",", snapshot[i].name,
                     (unsigned long long)snapshot[i].value);
        if (n > 0) {
            pos += (size_t)n;
        }
        first = false;
    }

    if (pos < len) {
        snprintf(buf + pos, len - pos, "}");
    } else {
        buf[len - 1] = '\0';
    }
}

/**
 * @brief Write a redirect stub so SoftAP clients use GET /api/ota instead.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if @p buf is unusable.
 */
esp_err_t sys_runtime_ota_stub_status(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* SoftAP OTA lives in fw_ota; use GET /api/ota for live status. */
    snprintf(buf, len, "{\"status\":\"see_/api/ota\"}");
    return ESP_OK;
}
