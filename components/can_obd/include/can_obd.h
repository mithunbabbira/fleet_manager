#pragma once
/*
 * Direct-CAN OBD-II transport (MCP2515), drop-in replacement for the
 * elm327_client text API on the feature/mcp2515-can branch.
 *
 * Protocol (bitrate + id width) is NOT hardcoded: by default the link
 * supervision task sweeps ISO 15765-4 candidates until an ECU answers
 * "0100", then persists the winner in NVS for fast next boot.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t can_obd_init(void);

/* Start link supervision (protocol detect + reprobe on link loss). */
esp_err_t can_obd_start(void);

/* True when an ECU has answered on the current protocol. */
bool can_obd_is_ready(void);

/*
 * Send an OBD command as hex text ("010C", "0902", "03") and return the
 * response payload as uppercase hex ("410C0C30") — same contract as
 * elm327_client_transact.
 *
 * ESP_ERR_NOT_SUPPORTED  AT commands (no ELM chip anymore)
 * ESP_ERR_INVALID_STATE  link not ready
 * ESP_ERR_NOT_FOUND      TX acked but no ECU response ("NO DATA")
 * ESP_ERR_TIMEOUT / ESP_FAIL   bus-level failure
 */
esp_err_t can_obd_transact(const char *cmd, char *resp, size_t resp_len,
                           uint32_t timeout_ms);

/* Active protocol, e.g. "ISO15765-4 CAN11/500"; "none" when link down. */
void can_obd_get_protocol(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
