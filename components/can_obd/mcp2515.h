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

esp_err_t mcp2515_init(int gpio_sck, int gpio_mosi, int gpio_miso, int gpio_cs,
                       int spi_clock_hz);

/* Reset + bit timing + RX filter + Normal mode.
 * filter_id/filter_mask apply to both RX buffers; ext selects 29-bit. */
esp_err_t mcp2515_configure(mcp_bitrate_t bitrate, bool ext,
                            uint32_t filter_id, uint32_t filter_mask);

/* Queue a frame in TXB0 and request transmission (does not wait for ACK). */
esp_err_t mcp2515_send(const mcp_can_frame_t *frame);

/* True once the last mcp2515_send was acked on the bus (TXREQ cleared). */
bool mcp2515_tx_done(void);

/* Abort a pending (unacked) transmission. */
void mcp2515_tx_abort(void);

/* Non-blocking receive. Returns true if a frame was read. */
bool mcp2515_receive(mcp_can_frame_t *frame);

/* TEC/REC/EFLG snapshot for diagnostics. */
void mcp2515_read_errors(uint8_t *tec, uint8_t *rec, uint8_t *eflg);

#ifdef __cplusplus
}
#endif
