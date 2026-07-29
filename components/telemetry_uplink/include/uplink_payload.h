#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPLINK_PID_FRESH_MS 15000u
#define UPLINK_SCHEMA_ID "1087"
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
    char ble_peer_address[24];
    char adapter_name[40];
    char obd_profile[40];
    char obd_protocol[48];
    uint32_t uptime_seconds;
    bool ble_connected;
    bool elm_ready;
    const char *poller_status; /* "on" / "paused" */
    uint64_t cmds_ok;
    uint64_t cmds_fail;
    uint64_t ble_reconnects;
    uint64_t blocked_cmds;
    uint64_t telemetry_drops;
    uplink_pid_view_t rpm;
    uplink_pid_view_t speed;
    uplink_pid_view_t coolant;
    uplink_pid_view_t throttle;
    uplink_pid_view_t voltage;
} uplink_snapshot_t;

/**
 * Build cloud event JSON into out (NUL-terminated).
 * Returns bytes written excluding NUL, or -1 on failure.
 * Missing/stale/!ok PIDs become JSON null with *_ok false.
 */
int uplink_payload_build(const uplink_snapshot_t *snap, char *out, size_t out_len);

/** True if a PID should be emitted as a numeric value. */
bool uplink_pid_is_fresh_ok(const uplink_pid_view_t *p);

#ifdef __cplusplus
}
#endif
