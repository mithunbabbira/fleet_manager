#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the heartbeat task (registers with the Task Watchdog Timer) and
 * pre-register the well-known metrics. Must be called once, after NVS init.
 */
esp_err_t sys_runtime_init(void);

/** Increment a named counter metric by 1, creating it on first use. */
void sys_runtime_metric_inc(const char *name);

/** Read the current value of a named counter metric (0 if unknown). */
uint64_t sys_runtime_metric_get(const char *name);

/** Render all known metrics as a flat JSON object into buf (NUL-terminated). */
void sys_runtime_metrics_snapshot_json(char *buf, size_t len);

/** Write a stub OTA status payload; full OTA is not implemented in v1. */
esp_err_t sys_runtime_ota_stub_status(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
