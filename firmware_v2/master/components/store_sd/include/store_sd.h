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

/**
 * @brief Create SPI2 and queue mutexes (idempotent).
 * @note Safe before CAN; shared with MCP soft-SPI CS discipline.
 */
esp_err_t store_sd_spi_lock_init(void);

/**
 * @brief Drive SD (+ MCP) CS idle-HIGH before any SPI traffic.
 * @note Required on shared bus so a floating SD CS cannot fight MCP2515 on MISO.
 */
void store_sd_spi_cs_idle_high(void);

/**
 * @brief Take the SPI2 mutex (MCP2515 vs SDSPI/FatFS).
 * @note Timeout returns ESP_ERR_TIMEOUT; pair with store_sd_spi_unlock.
 */
esp_err_t store_sd_spi_lock(uint32_t timeout_ms);

/** @brief Release the SPI2 mutex. */
void store_sd_spi_unlock(void);

/**
 * @brief Mount FAT on SDSPI (CS from Kconfig); load uplink queue meta.
 * @note SPI2 may already be up; holds SPI lock during mount. Non-fatal if card missing.
 */
esp_err_t store_sd_init(void);

/** @brief True after a successful store_sd_init mount. */
bool store_sd_is_mounted(void);

/**
 * @brief Snapshot mount flag, queue depth/bytes, and last error.
 * @note Takes queue then SPI locks briefly when mounted.
 */
esp_err_t store_sd_get_status(store_sd_status_t *out);

/**
 * @brief Append one NDJSON line to the microSD uplink queue; drop oldest if full.
 * @note Holds queue + SPI locks; newline appended by this API.
 */
esp_err_t store_sd_enqueue_line(const char *line, size_t line_len);

/**
 * @brief Peek up to @p max_lines from queue head into @p buf (NUL-terminated).
 * @note Does not advance head; @p *out_byte_span includes newlines for later ack.
 */
esp_err_t store_sd_peek_lines(char *buf, size_t buf_len, size_t max_lines,
                              size_t *out_lines, size_t *out_byte_span);

/**
 * @brief Advance queue head after a successful uplink batch POST.
 * @note Holds queue + SPI locks; may compact/truncate when empty.
 */
esp_err_t store_sd_ack_bytes(size_t byte_span, size_t lines);

#ifdef __cplusplus
}
#endif
