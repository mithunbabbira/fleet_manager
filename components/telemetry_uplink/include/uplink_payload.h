#pragma once

/*
 * Cloud telemetry JSON helpers (no ESP-IDF deps — host-testable).
 *
 * Trafyn nc-events-api expects each HTTP body item as:
 *   { "schemaId": "<id>", "payload": { ...device fields... } }
 * Batch POST is a JSON array of those objects.
 *
 * POST destination and schema id are compile-time constants (baked into the
 * .bin). They are not SoftAP/NVS-configurable today — change here and rebuild
 * to point at a different environment.
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
 * Not stored in NVS — editing this string requires a new firmware image.
 */
#define UPLINK_URL "https://api.trafyn.info/nc-events-api/v2/messages"

typedef struct {
    bool valid;
    bool ok;
    double value;
    char raw[48];
    uint32_t age_ms;
} uplink_pid_view_t;

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
} uplink_snapshot_t;

/**
 * Build cloud event JSON into out (NUL-terminated).
 * Returns bytes written excluding NUL, or -1 on failure.
 * Missing/stale/!ok PIDs omit value keys with *_ok false.
 */
int uplink_payload_build(const uplink_snapshot_t *snap, char *out, size_t out_len);

/** Build only the payload object `{...}` (no schemaId wrapper). */
int uplink_payload_build_payload(const uplink_snapshot_t *snap, char *out, size_t out_len);

/**
 * Build one queued event line: {"payload":{...},"queued_at_ms":N}
 * payload_json is the object from uplink_payload_build_payload.
 */
int uplink_payload_build_queued_event(const char *payload_json, uint64_t queued_at_ms,
                                      char *out, size_t out_len);

/**
 * Build batch POST body from NDJSON queued lines
 * (`{"payload":{...},"queued_at_ms":N}`). HTTP body is a JSON array:
 * [{"schemaId":"1087","payload":{...}}, ...]. queued_at_ms stays on SD only.
 * Inner payload (including gps_ok/lat/lng) is copied unchanged.
 */
int uplink_payload_build_batch(const char *events_blob, size_t n_events, char *out,
                               size_t out_len);

/** True if a PID should be emitted as a numeric value. */
bool uplink_pid_is_fresh_ok(const uplink_pid_view_t *p);

/** Store/send this tick if any OBD PID is fresh, or GNSS has a live fix. */
bool uplink_should_enqueue(bool have_fresh_pid, bool gps_ok);

#define UPLINK_GPS_ONLY_MIN_MOVE_M 50.0
#define UPLINK_GPS_ONLY_HEARTBEAT_MS 300000ULL /* 5 min while parked */

/** Great-circle distance in metres. */
double uplink_gps_distance_m(double lat1, double lng1, double lat2, double lng2);

/**
 * GPS-only ticks: send if first fix, moved >= UPLINK_GPS_ONLY_MIN_MOVE_M,
 * or heartbeat elapsed. have_last false → always send.
 */
bool uplink_gps_only_worth_sending(bool have_last, double last_lat, double last_lng,
                                   uint64_t last_ms, double lat, double lng, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
