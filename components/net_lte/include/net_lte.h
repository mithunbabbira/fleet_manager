#pragma once

#include "esp_err.h"
#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
