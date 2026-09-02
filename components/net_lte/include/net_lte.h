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
 * @brief Start UART1 + background modem bring-up (and GPS task if enabled).
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED if disabled, or init errors.
 * @note Non-blocking: AT probing runs in lte_bringup. Pins: TX=CONFIG TX GPIO, RX=RX GPIO.
 */
esp_err_t net_lte_start(void);

/**
 * @brief Copy cached modem status (SIM/reg/CSQ/APN/IP/last_error).
 */
esp_err_t net_lte_get_status(net_lte_status_t *out);

/**
 * @brief Re-query CPIN/CSQ/COPS/CxREG/CGATT over AT (takes UART mutex each command).
 */
esp_err_t net_lte_refresh(void);

/**
 * @brief Blocking self-test: AT → SIM → register → PDP → optional ping; fills @p report.
 * @note Holds UART mutex up to ~120s. PDP "FAIL" with IP present often means already active.
 */
esp_err_t net_lte_selftest(char *report, size_t report_len);

/**
 * @brief Soft reconnect helper (currently re-enters net_lte_start).
 */
esp_err_t net_lte_reconnect(void);

typedef struct {
    bool gps_ok;
    double lat;
    double lng;
    uint32_t age_ms;
} net_lte_gps_t;

/**
 * @brief Copy age-gated GNSS cache; gps_ok false if never fixed or older than max age.
 * @note Uses s_gps_mutex — does not wait on in-flight HTTP.
 */
esp_err_t net_lte_gps_get(net_lte_gps_t *out);

typedef struct {
    int http_status; /* 0 if unknown / transport failed before status */
    char error[96];
} net_lte_http_result_t;

/**
 * @brief HTTPS POST JSON via QHTTP (2xx = success). Serializes on UART mutex.
 */
esp_err_t net_lte_http_post(const char *url, const char *body, net_lte_http_result_t *out);

typedef struct {
    const char *authorization;   /* NULL or "" → omit header */
    const char *system_user_id;  /* NULL or "" → omit header */
} net_lte_http_req_headers_t;

/**
 * @brief HTTPS POST with optional custom headers and optional response body capture.
 * @note When resp_buf non-NULL, streams QHTTPREAD into buffer; else short-drain.
 */
esp_err_t net_lte_http_post_recv(const char *url, const char *body,
                                 const net_lte_http_req_headers_t *hdr,
                                 char *resp_buf, size_t resp_buf_len, size_t *resp_len,
                                 net_lte_http_result_t *out);

/**
 * @brief HTTPS GET into a small buffer (manifest-sized). Holds mutex up to ~300s.
 */
esp_err_t net_lte_http_get(const char *url, char *buf, size_t buf_len, size_t *out_len,
                          net_lte_http_result_t *out);

typedef esp_err_t (*net_lte_http_chunk_cb_t)(const uint8_t *data, size_t len, void *ctx);

/**
 * @brief HTTPS GET streaming for large bodies (OTA .bin); invokes chunk callback.
 * @note Does not buffer the full body in RAM.
 */
esp_err_t net_lte_http_get_stream(const char *url, net_lte_http_chunk_cb_t cb, void *ctx,
                                  size_t *content_length_out, net_lte_http_result_t *out);

/**
 * @brief Pause GPS (and other BG AT) while LTE OTA owns the modem UART.
 * @note Only gps_task honors this today; other at_transact callers still contend via mutex.
 */
void net_lte_suspend_bg_at(bool suspend);

#ifdef __cplusplus
}
#endif
