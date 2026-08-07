#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool mounted;
    uint32_t depth;
    uint64_t pending_bytes;
    uint64_t head_offset;
    char last_error[80];
} store_sd_status_t;

/** Create SPI bus mutex (safe to call before CAN). */
esp_err_t store_sd_spi_lock_init(void);

/**
 * Drive SD CS (and optionally MCP CS) idle-HIGH before any SPI traffic.
 * Required on a shared bus: a floating SD CS lets the card fight MCP2515 on MISO.
 */
void store_sd_spi_cs_idle_high(void);

/** Serialize SPI2 use between MCP2515 and SDSPI/FatFS. */
esp_err_t store_sd_spi_lock(uint32_t timeout_ms);
void store_sd_spi_unlock(void);

/**
 * Mount FAT on SDSPI (CS from Kconfig). SPI2 may already be initialized by MCP2515.
 * Returns ESP_OK if mounted, ESP_ERR_NOT_FOUND / other on failure (non-fatal for boot).
 */
esp_err_t store_sd_init(void);

bool store_sd_is_mounted(void);
esp_err_t store_sd_get_status(store_sd_status_t *out);

/**
 * Append one NDJSON line (object without trailing newline). Drops oldest when full.
 * line_len is strlen; newline is written by this API.
 */
esp_err_t store_sd_enqueue_line(const char *line, size_t line_len);

/**
 * Peek up to max_lines from head into buf (NUL-terminated, lines separated by '\\n').
 * *out_byte_span is bytes to ack (including newlines), *out_lines is count.
 */
esp_err_t store_sd_peek_lines(char *buf, size_t buf_len, size_t max_lines,
                              size_t *out_lines, size_t *out_byte_span);

/** Advance head after successful batch POST. */
esp_err_t store_sd_ack_bytes(size_t byte_span, size_t lines);

#ifdef __cplusplus
}
#endif
