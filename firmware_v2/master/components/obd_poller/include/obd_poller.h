#pragma once
/*
 * Thin Mode-01 poller for firmware_v2 (rpm/speed/coolant/throttle).
 * Polls only when can_obd_is_ready(); no profile_store / SoftAP.
 */

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    bool ok;
    double value;
    char raw[48];
    uint32_t age_ms;
} obd_pid_view_t;

typedef struct {
    char protocol[48];
    bool can_ready;
    bool enabled;
    uint64_t cmds_ok;
    uint64_t cmds_fail;
    uint32_t uptime_seconds;
    obd_pid_view_t rpm;
    obd_pid_view_t speed;
    obd_pid_view_t coolant;
    obd_pid_view_t throttle;
} obd_poller_snapshot_t;

esp_err_t obd_poller_init(void);
esp_err_t obd_poller_start(void);

/** @brief Fill snapshot with current PID ages (mutex). */
esp_err_t obd_poller_get_snapshot(obd_poller_snapshot_t *out);

/** @brief Any PID with valid+ok and age_ms ≤ 15 s. */
bool obd_poller_has_fresh_pid(const obd_poller_snapshot_t *snap);

#ifdef __cplusplus
}
#endif
