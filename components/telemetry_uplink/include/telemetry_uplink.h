#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    uint16_t interval_s;
    char device_id[40];
    char node_id[40];
} telemetry_uplink_config_t;

typedef struct {
    bool ok;
    bool skipped;
    int http_status;
    uint64_t ts_ms;
    char reason[48];
    char error[96];
} telemetry_uplink_last_t;

typedef struct {
    bool sd_mounted;
    uint32_t queue_depth;
    uint64_t queue_bytes;
    char drain_error[80];
} telemetry_uplink_queue_t;

typedef struct {
    telemetry_uplink_config_t config;
    telemetry_uplink_last_t last;
    telemetry_uplink_queue_t queue;
    const char *url;
    const char *schema_id;
} telemetry_uplink_status_t;

esp_err_t telemetry_uplink_start(void);

esp_err_t telemetry_uplink_get_config(telemetry_uplink_config_t *out);
esp_err_t telemetry_uplink_set_config(const telemetry_uplink_config_t *in);
esp_err_t telemetry_uplink_get_status(telemetry_uplink_status_t *out);

/** Force one produce+enqueue (and kick drain). Same gating as the interval task. */
esp_err_t telemetry_uplink_send_now(void);

/** Lab helper: enqueue a minimal dummy event (no CAN gate) and kick drain. */
esp_err_t telemetry_uplink_queue_test_enqueue(void);

#ifdef __cplusplus
}
#endif
