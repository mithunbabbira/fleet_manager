#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char node_id[40];
    char url[256];
    bool enabled;
    int last_http_status;
    char last_error[96];
    uint64_t last_send_ms;
    uint32_t send_ok;
    uint32_t send_fail;
    uint32_t skip_no_id;
    uint32_t skip_no_gps;
    uint32_t skip_no_obd;
    uint32_t skip_gate;
    uint32_t skip_no_host;
    uint32_t queue_enqueued;
    uint32_t queue_drained;
    uint32_t queue_depth;
} uplink_status_t;

esp_err_t uplink_init(void);
esp_err_t uplink_start(void);
esp_err_t uplink_get_status(uplink_status_t *out);
esp_err_t uplink_set_node_id(const char *node_id);
esp_err_t uplink_get_node_id(char *out, size_t out_len);

/** Persist telemetry POST URL in NVS (overrides CONFIG_UPLINK_URL after reboot). */
esp_err_t uplink_set_post_url(const char *url);

/**
 * Force one produce tick: collect OBD 1087 + Zigbee 1088 + GPS 1089 into one JSON array POST.
 * GPS bypasses heartbeat/move gate; still needs gps_ok for 1089.
 */
esp_err_t uplink_once(void);

/**
 * Lab-only: POST one synthetic 1089 envelope as a JSON array (no live GPS required).
 * Uses provisioned device_id / node_id → virtual GPS ids.
 */
esp_err_t uplink_lab_post(void);

#ifdef __cplusplus
}
#endif
