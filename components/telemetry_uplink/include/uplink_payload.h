#pragma once

/*
 * Cloud telemetry JSON helpers (no ESP-IDF deps — host-testable).
 *
 * Each uplink tick emits 0..N events. Envelope fields sit at the top level:
 *   { "device_id", "node_id", "schemaId", "ts_ms", "payload": { ... } }
 *
 * Live POST: single object when count==1, JSON array when count>1.
 * Batch POST (SD drain): always a JSON array.
 *
 * POST destination defaults from UPLINK_URL below. OBD schemaId can be
 * overridden in NVS (serial / Carrier Console).
 */

#include "uplink_schema.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Drop OBD PID values older than this from the payload (stale / missing). */
#define UPLINK_PID_FRESH_MS 15000u

/**
 * HTTPS endpoint for telemetry uplink (EC200U QHTTP POST via net_lte).
 *
 * Factory default when NVS uplink_url is empty. Override at runtime via serial
 * `uplink url` or Carrier Console (persists in NVS; survives OTA).
 */
#define UPLINK_URL "https://api.trafyn.info/nc-events-api/v2/messages"

/* 1 OBD + 1 GPS + host readings. Excess readings roll into the next tick. */
#define UPLINK_MAX_EVENTS_PER_TICK 12

typedef struct {
    bool valid;
    bool ok;
    double value;
    char raw[48];
    uint32_t age_ms;
} uplink_pid_view_t;

#define UPLINK_MAX_HOSTS 4
#define UPLINK_MAX_HOST_READINGS 8

/* Must match telemetry_host_reading_t / the manifest limits — a shorter buffer
 * here would truncate the reading key that becomes the cloud metric name. */
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
    uint64_t ts_ms;
    uint8_t host_count;
    uplink_host_report_t hosts[UPLINK_MAX_HOSTS];
} uplink_snapshot_t;

typedef struct {
    char device_id[40];
    char node_id[40];
    const char *schema_id;
    uint64_t ts_ms;
    char payload_json[768];
} uplink_event_t;

typedef struct {
    bool include_obd;
    bool include_gps;
    const char *obd_schema_id; /* NULL → UPLINK_SCHEMA_OBD */
} uplink_emit_ctx_t;

bool uplink_pid_is_fresh_ok(const uplink_pid_view_t *p);

/** @brief Any PID fresh within UPLINK_PID_FRESH_MS. */
bool uplink_snapshot_has_fresh_pid(const uplink_snapshot_t *snap);

/** @brief At least one host reading with valid==true. */
bool uplink_snapshot_has_valid_host_reading(const uplink_snapshot_t *snap);

/** @brief Enqueue if fresh OBD PID or live GPS (legacy helper). */
bool uplink_should_enqueue(bool have_fresh_pid, bool gps_ok);

/** @brief Tick worth producing when any sub-event would be emitted. */
bool uplink_tick_worth_producing(bool have_fresh_pid, bool gps_ok, bool gps_worth,
                                 const uplink_snapshot_t *snap);

#define UPLINK_GPS_ONLY_MIN_MOVE_M 50.0
#define UPLINK_GPS_ONLY_HEARTBEAT_MS 300000ULL /* 5 min while parked */

double uplink_gps_distance_m(double lat1, double lng1, double lat2, double lng2);

/** @brief GPS-only: first fix, ≥50 m move, or 5 min heartbeat. */
bool uplink_gps_only_worth_sending(bool have_last, double last_lat, double last_lng,
                                   uint64_t last_ms, double lat, double lng, uint64_t now_ms);

/** @brief carrier-042 → gps-042; node-042 → node-gps-042 (suffix after last '-'). */
void uplink_virtual_gps_ids(const char *carrier_device_id, const char *carrier_node_id,
                            char *gps_device_id, size_t gps_dev_len, char *gps_node_id,
                            size_t gps_node_len);

/** @brief node-{host_device_id} */
void uplink_host_node_id(const char *host_device_id, char *node_id, size_t node_len);

int uplink_payload_build_obd_payload(const uplink_snapshot_t *snap, char *out, size_t out_len);
int uplink_payload_build_gps_payload(const uplink_snapshot_t *snap, char *out, size_t out_len);
int uplink_payload_build_host_reading_payload(const uplink_host_reading_t *reading, char *out,
                                              size_t out_len);

int uplink_events_from_snapshot(const uplink_snapshot_t *snap, const uplink_emit_ctx_t *ctx,
                                uplink_event_t *out, uint8_t max_out, uint8_t *count);

int uplink_event_serialize(const uplink_event_t *ev, char *out, size_t out_len);

/** @brief Full envelope + queued_at_ms for SD NDJSON line. */
int uplink_event_serialize_queued(const uplink_event_t *ev, uint64_t queued_at_ms, char *out,
                                  size_t out_len);

/** @brief Live POST body: one object or array. */
int uplink_events_serialize_live(const uplink_event_t *evs, uint8_t count, char *out,
                                 size_t out_len);

/**
 * @brief Batch array from SD queue lines (strips queued_at_ms).
 * @note schemaId is already on each queued line — not re-wrapped.
 */
int uplink_payload_build_batch(const char *events_blob, size_t n_events, char *out,
                               size_t out_len);

#ifdef __cplusplus
}
#endif
