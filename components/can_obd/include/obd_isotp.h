#pragma once
/*
 * Minimal ISO 15765-2 (ISO-TP) framing for OBD-II requests/responses.
 * Pure C, no ESP-IDF dependencies: host-unit-testable.
 *
 * Scope: requests always fit a single frame (OBD mode+PID <= 7 bytes).
 * Responses may be single frame or first-frame + consecutive frames
 * (e.g. VIN 0902, long DTC lists). Flow-control TX is signalled to the
 * caller, which owns the CAN bus.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OBD_ISOTP_MAX_PAYLOAD 64

/* Build a padded 8-byte single-frame from a hex command string ("010C",
 * "0902", "03"). Returns 0 on success, -1 on bad input or payload > 7. */
int obd_isotp_build_sf(const char *cmd_hex, uint8_t out[8]);

typedef enum {
    OBD_ISOTP_RX_IGNORED = 0,   /* not an ISO-TP frame we care about */
    OBD_ISOTP_RX_COMPLETE,      /* full payload assembled */
    OBD_ISOTP_RX_NEED_FC,       /* first frame seen: caller must send flow control */
    OBD_ISOTP_RX_IN_PROGRESS,   /* consecutive frame consumed, more expected */
    OBD_ISOTP_RX_ERROR,         /* sequence error: reset and give up */
} obd_isotp_rx_status_t;

typedef struct {
    uint8_t buf[OBD_ISOTP_MAX_PAYLOAD];
    size_t expected_len;
    size_t got_len;
    uint8_t next_sn;      /* expected consecutive-frame sequence number */
    bool in_progress;
    uint32_t src_id;      /* CAN id of the ECU we locked onto (0 = none) */
} obd_isotp_rx_t;

void obd_isotp_rx_reset(obd_isotp_rx_t *rx);

/* Feed one received CAN frame (data + dlc, with its CAN id).
 * When a multi-frame transfer is in progress, frames from other ids are
 * ignored. Returns the resulting status. */
obd_isotp_rx_status_t obd_isotp_rx_feed(obd_isotp_rx_t *rx, uint32_t can_id,
                                        const uint8_t *data, uint8_t dlc);

/* After OBD_ISOTP_RX_COMPLETE: write payload as uppercase hex ("410C0C30").
 * Returns chars written (excl. NUL) or -1 if out is too small. */
int obd_isotp_payload_hex(const obd_isotp_rx_t *rx, char *out, size_t out_len);

/* Fill a padded 8-byte flow-control frame (ClearToSend, no block limit). */
void obd_isotp_build_fc(uint8_t out[8]);

#ifdef __cplusplus
}
#endif
