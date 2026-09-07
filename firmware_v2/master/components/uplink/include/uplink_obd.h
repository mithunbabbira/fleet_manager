#pragma once
/*
 * Host-testable OBD (schema 1087) payload helpers for firmware_v2 uplink.
 */

#include "uplink_envelope.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPLINK_PID_FRESH_MS 15000u

typedef struct {
    bool valid;
    bool ok;
    double value;
    char raw[48];
    uint32_t age_ms;
} uplink_pid_view_t;

typedef struct {
    char obd_profile[40];
    char obd_protocol[48];
    uint32_t uptime_seconds;
    const char *poller_status; /* "on" / "paused" */
    uint64_t cmds_ok;
    uint64_t cmds_fail;
    uplink_pid_view_t rpm;
    uplink_pid_view_t speed;
    uplink_pid_view_t coolant;
    uplink_pid_view_t throttle;
} uplink_obd_snapshot_t;

bool uplink_pid_is_fresh_ok(const uplink_pid_view_t *p);
bool uplink_obd_has_fresh_pid(const uplink_obd_snapshot_t *snap);

/** @brief Build 1087 payload object. Returns bytes written or -1. */
int uplink_build_obd_payload(const uplink_obd_snapshot_t *snap, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
