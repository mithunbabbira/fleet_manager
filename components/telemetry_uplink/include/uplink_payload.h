#pragma once

/*
 * Cloud telemetry JSON helpers (no ESP-IDF deps — host-testable).
 *
 * Trafyn nc-events-api expects each HTTP body item as:
 *   { "schemaId": "<id>", "payload": { ...device fields... } }
 * Batch POST is a JSON array of those objects.
 *
 * POST destination and schema id default from UPLINK_* macros below.
 * Runtime overrides live in NVS (serial / Carrier Console); firmware defaults
 * apply when NVS is empty (same pattern as OTA check URL).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Drop OBD PID values older than this from the payload (stale / missing). */
#define UPLINK_PID_FRESH_MS 15000u

/**
 * Trafyn event schema for fleet telematics samples.
 * Wrapped around every payload object in live and batch POSTs.
 */
#define UPLINK_SCHEMA_ID "1087"

/**
 * HTTPS endpoint for telemetry uplink (EC200U QHTTP POST via net_lte).
 *
 * Used by telemetry_uplink.c for:
 *   - live single-event POST when microSD is not mounted
 *   - batch POST when draining the SD queue (JSON array body)
 *
 * Factory default when NVS uplink_url is empty. Override at runtime via serial
 * `uplink url` or Carrier Console (persists in NVS; survives OTA).
 */
#define UPLINK_URL "https://api.trafyn.info/nc-events-api/v2/messages"

typedef struct {
    bool valid;
    bool ok;
    double value;
    char raw[48];
    uint32_t age_ms;
} uplink_pid_view_t;

#define UPLINK_MAX_HOSTS 4
#define UPLINK_MAX_HOST_READINGS 8

typedef struct {
    char key[24];
    double value;
    char unit[8];
    bool valid;
} uplink_host_reading_t;

typedef struct {
    char device_id[32];
    char host_type[32];
    uint16_t host_type_id;
    uint8_t reading_count;
    uplink_host_reading_t readings[UPLINK_MAX_HOST_READINGS];
    uint64_t ts_ms;
} uplink_host_report_t;

typedef struct {
    char device_id[40];
    char node_id[40];
    char obd_profile[40];
    char obd_protocol[48];
    uint32_t uptime_seconds;
    const char *poller_status; /* "on" / "paused" */
    uint64_t cmds_ok;
    uint64_t cmds_fail;
    uint64_t blocked_cmds;
    uint64_t telemetry_drops;
    uplink_pid_view_t rpm;
    uplink_pid_view_t speed;
    uplink_pid_view_t coolant;
    uplink_pid_view_t throttle;
    uplink_pid_view_t voltage;
    bool gps_ok;
    double lat;
    double lng;
    uint64_t ts_ms; /* capture time (esp_timer ms); sent in payload for replay */
    uint8_t host_count;
    uplink_host_report_t hosts[UPLINK_MAX_HOSTS];
} uplink_snapshot_t;

/** @brief Live body {"schemaId","payload"}; @p schema_id NULL → UPLINK_SCHEMA_ID. */
int uplink_payload_build(const uplink_snapshot_t *snap, const char *schema_id, char *out,
                         size_t out_len);

/** @brief Inner payload object only (PIDs, gps, device/node ids). */
int uplink_payload_build_payload(const uplink_snapshot_t *snap, char *out, size_t out_len);

/** @brief NDJSON queue line with queued_at_ms. */
int uplink_payload_build_queued_event(const char *payload_json, uint64_t queued_at_ms,
                                      char *out, size_t out_len);

/**
 * @brief Batch array re-wrapping each queued payload with schemaId.
 * @param schema_id NULL → UPLINK_SCHEMA_ID
 */
int uplink_payload_build_batch(const char *events_blob, size_t n_events, const char *schema_id,
                               char *out, size_t out_len);

bool uplink_pid_is_fresh_ok(const uplink_pid_view_t *p);

/** @brief Enqueue if fresh OBD PID or live GPS. */
bool uplink_should_enqueue(bool have_fresh_pid, bool gps_ok);

#define UPLINK_GPS_ONLY_MIN_MOVE_M 50.0
#define UPLINK_GPS_ONLY_HEARTBEAT_MS 300000ULL /* 5 min while parked */

double uplink_gps_distance_m(double lat1, double lng1, double lat2, double lng2);

/** @brief GPS-only: first fix, ≥50 m move, or 5 min heartbeat. */
bool uplink_gps_only_worth_sending(bool have_last, double last_lat, double last_lng,
                                   uint64_t last_ms, double lat, double lng, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
