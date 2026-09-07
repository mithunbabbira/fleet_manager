#pragma once
/*
 * Direct-CAN OBD-II transport for the MCP2515 on feature/mcp2515-can.
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

/** @brief Create bus mutex; soft-SPI init + MCP2515 detect (CANSTAT). */
esp_err_t can_obd_init(void);

/** @brief Start link_task (detect / reprobe). */
esp_err_t can_obd_start(void);

/** @brief True after mcp2515_init saw a valid CANSTAT (chip present; no ECU required). */
bool can_obd_mcp_present(void);

/** @brief True when an ECU answered protocol detect (needs vehicle / simulator). */
bool can_obd_is_ready(void);

/**
 * @brief Hex OBD cmd → hex ISO-TP payload (single-flight mutex).
 * @return ESP_FAIL TX not acked (offline bus); ESP_ERR_NOT_FOUND acked but no data;
 *         ESP_ERR_TIMEOUT mutex busy.
 */
esp_err_t can_obd_transact(const char *cmd, char *resp, size_t resp_len,
                           uint32_t timeout_ms);

/** @brief Active protocol name or "none". */
void can_obd_get_protocol(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
