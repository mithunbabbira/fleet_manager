#pragma once
/* Register-level MCP2515 driver (SPI), internal to can_obd. */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    bool ext;       /* 29-bit extended id */
    uint8_t dlc;
    uint8_t data[8];
} mcp_can_frame_t;

typedef enum {
    MCP_BITRATE_500K = 0,
    MCP_BITRATE_250K,
} mcp_bitrate_t;

/**
 * @brief GPIO + RESET + verify config-mode CANSTAT (detect).
 */
esp_err_t mcp2515_init(int gpio_sck, int gpio_mosi, int gpio_miso, int gpio_cs,
                       int spi_clock_hz);

/**
 * @brief Bit timing, masks, RXF0/RXF2, Normal mode.
 * @warning Unused RXFn left at reset defaults — program all filters (C1).
 */
esp_err_t mcp2515_configure(mcp_bitrate_t bitrate, bool ext,
                            uint32_t filter_id, uint32_t filter_mask);

/** @brief Load TXB0 + RTS (does not wait for ACK). */
esp_err_t mcp2515_send(const mcp_can_frame_t *frame);

/**
 * @brief TXREQ clear — finished, aborted, or bus-off cleared request (not pure ACK).
 */
bool mcp2515_tx_done(void);

void mcp2515_tx_abort(void);

bool mcp2515_receive(mcp_can_frame_t *frame);

void mcp2515_read_errors(uint8_t *tec, uint8_t *rec, uint8_t *eflg);

#ifdef __cplusplus
}
#endif
