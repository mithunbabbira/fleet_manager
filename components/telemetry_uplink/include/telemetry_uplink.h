#pragma once

/*
 * Fleet telemetry uplink over LTE.
 *
 * Flow:
 *   1) Produce task samples OBD + GNSS on an interval (NVS uplink_iv).
 *   2) Prefer enqueue to microSD (durable queue).
 *   3) Drain task batch-POSTs queued events to UPLINK_URL (see uplink_payload.h).
 *   4) If SD is missing, produce does a live single POST to the same URL.
 *
 * device_id / node_id / enable / interval / POST URL / schema live in NVS.
 * Factory defaults for URL/schema are in uplink_payload.h when NVS is empty.
 */

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
    /** Effective POST URL (NVS override or compile-time default). */
    const char *url;
    /** Effective schema id (NVS override or compile-time default). */
    const char *schema_id;
} telemetry_uplink_status_t;

/** @brief True when device_id and node_id are set in NVS (required before uplink/OTA). */
bool telemetry_uplink_is_provisioned(void);

/**
 * @brief Set device_id + node_id together; persists NVS and syncs OTA device_id.
 * @note Does not enable uplink — call separately after provisioning.
 */
esp_err_t telemetry_uplink_provision(const char *device_id, const char *node_id);

/** @brief Persist telemetry POST URL (NVS uplink_url). */
esp_err_t telemetry_uplink_set_post_url(const char *url);

/** @brief Persist schema id (NVS uplink_schema). */
esp_err_t telemetry_uplink_set_schema_id(const char *schema_id);

/**
 * @brief Start cache + tick (+ drain if SD mounted at boot). Idempotent.
 * @note Drain task is not created if SD mounts later.
 */
esp_err_t telemetry_uplink_start(void);

esp_err_t telemetry_uplink_get_config(telemetry_uplink_config_t *out);
/**
 * @brief Validate, save NVS elm/uplink_*, update RAM (shares uplink_did with OTA).
 */
esp_err_t telemetry_uplink_set_config(const telemetry_uplink_config_t *in);
esp_err_t telemetry_uplink_get_status(telemetry_uplink_status_t *out);

/** @brief One produce; notify drain and best-effort drain_once if applicable. */
esp_err_t telemetry_uplink_send_now(void);

/** @brief Lab: enqueue minimal event and kick drain (bypasses PID/GPS gates). */
esp_err_t telemetry_uplink_queue_test_enqueue(void);

#ifdef __cplusplus
}
#endif
