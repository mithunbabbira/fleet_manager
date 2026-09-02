#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start TWDT heartbeat task and pre-register well-known metrics.
 * @note Call once after NVS init. Tolerates TWDT already started by IDF startup.
 * @return ESP_OK, ESP_ERR_NO_MEM, or a TWDT init error.
 */
esp_err_t sys_runtime_init(void);

/**
 * @brief Increment a named counter by 1 (creates the slot on first use).
 * @param name Metric key (truncated to 23 chars); NULL is a no-op.
 * @note Thread-safe critical section; silent no-op if the 16-slot table is full.
 */
void sys_runtime_metric_inc(const char *name);

/**
 * @brief Read the current value of a named counter.
 * @param name Metric key; NULL or unknown → 0.
 * @return Counter value (0 if missing).
 */
uint64_t sys_runtime_metric_get(const char *name);

/**
 * @brief Render all known metrics as a flat JSON object into @p buf.
 * @param buf Destination (NUL-terminated if @p len > 0).
 * @param len Capacity; truncates safely if too small.
 * @note Copies under lock, then formats outside the critical section.
 */
void sys_runtime_metrics_snapshot_json(char *buf, size_t len);

/**
 * @brief Stub SoftAP OTA status string (compat placeholder).
 * @note Live OTA status lives in fw_ota / GET /api/ota.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if @p buf is NULL/empty.
 */
esp_err_t sys_runtime_ota_stub_status(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
