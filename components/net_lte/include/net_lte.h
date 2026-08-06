#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    bool uart_ok;
    bool link_up;
    bool ip_up;
    bool sim_ready;
    bool registered;
    bool attached;
    int csq;       /* raw AT+CSQ value 0..31, 99 = unknown */
    int rssi_dbm;  /* derived from csq, 0 if unknown */
    char ip[16];
    char apn[64];
    char operator_name[32];
    char last_error[64];
    char ati[64];
} net_lte_status_t;

/**
 * Start LTE bring-up on UART1 (default GPIO17 TX / GPIO16 RX).
 *
 * Phase 1: UART AT ping (AT / ATI / CPIN). PPP comes later.
 * Returns ESP_ERR_NOT_SUPPORTED when CONFIG_NET_LTE_ENABLE is unset.
 */
esp_err_t net_lte_start(void);

esp_err_t net_lte_get_status(net_lte_status_t *out);

/** Re-query live modem state (CSQ, operator, registration) over UART. */
esp_err_t net_lte_refresh(void);

/**
 * Modem-level internet self-test: wait for registration, activate a PDP
 * context, obtain an IP, and ping 8.8.8.8 — all via Quectel AT (no PPP).
 * Writes a human-readable multi-line report into `report`.
 */
esp_err_t net_lte_selftest(char *report, size_t report_len);

esp_err_t net_lte_reconnect(void);

typedef struct {
    bool gps_ok;
    double lat;
    double lng;
    uint32_t age_ms;
} net_lte_gps_t;

/** Copy latest GNSS cache. gps_ok false if never fixed or older than max age. */
esp_err_t net_lte_gps_get(net_lte_gps_t *out);

typedef struct {
    int http_status; /* 0 if unknown / transport failed before status */
    char error[96];
} net_lte_http_result_t;

/**
 * HTTPS POST JSON body via Quectel QHTTP* (no PPP).
 * Treats HTTP 2xx as success. Serializes on the shared UART AT mutex.
 */
esp_err_t net_lte_http_post(const char *url, const char *body, net_lte_http_result_t *out);

/**
 * HTTPS GET into a buffer (for small bodies e.g. OTA manifest JSON).
 * Writes up to buf_len-1 bytes and NUL-terminates when treating as text.
 * Sets *out_len to bytes copied (not including NUL).
 */
esp_err_t net_lte_http_get(const char *url, char *buf, size_t buf_len, size_t *out_len,
                          net_lte_http_result_t *out);

typedef esp_err_t (*net_lte_http_chunk_cb_t)(const uint8_t *data, size_t len, void *ctx);

/**
 * HTTPS GET streaming: after QHTTPGET, reads body via QHTTPREAD and invokes
 * cb for each chunk. Does not buffer the full body in RAM.
 * If content_length_out is non-NULL, sets advertised Content-Length (0 if unknown).
 */
esp_err_t net_lte_http_get_stream(const char *url, net_lte_http_chunk_cb_t cb, void *ctx,
                                  size_t *content_length_out, net_lte_http_result_t *out);

#ifdef __cplusplus
}
#endif
