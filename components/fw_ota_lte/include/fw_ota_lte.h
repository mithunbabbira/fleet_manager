#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FW_OTA_LTE_IDLE = 0,
    FW_OTA_LTE_CHECKING,
    FW_OTA_LTE_DOWNLOADING,
    FW_OTA_LTE_NO_UPDATE,
    FW_OTA_LTE_FAILED,
    FW_OTA_LTE_REBOOTING,
} fw_ota_lte_phase_t;

typedef struct {
    char manifest_url[192];
    char channel[24];
    char device_id[40];
    bool force;
} fw_ota_lte_config_t;

typedef struct {
    fw_ota_lte_phase_t phase;
    char error[96];
    char manifest_version[40];
    char applied_version[40];
    int http_status;
    size_t bytes_downloaded;
} fw_ota_lte_status_t;

esp_err_t fw_ota_lte_init(void);
esp_err_t fw_ota_lte_get_config(fw_ota_lte_config_t *out);
esp_err_t fw_ota_lte_set_config(const fw_ota_lte_config_t *in);
esp_err_t fw_ota_lte_get_status(fw_ota_lte_status_t *out);

/** Blocking check+update (call from worker task, not httpd). */
esp_err_t fw_ota_lte_run(void);

/** Spawn background task to run fw_ota_lte_run(). Returns ESP_ERR_INVALID_STATE if busy. */
esp_err_t fw_ota_lte_start_background(void);

/**
 * Start long-lived auto-check (wait for LTE → check → sleep interval → repeat).
 * No-op / ESP_ERR_NOT_SUPPORTED when CONFIG_FW_OTA_LTE_AUTO_CHECK is unset.
 * Auto path always uses force=false.
 */
esp_err_t fw_ota_lte_start_auto(void);

/** True while LTE OTA check/download/reboot is in progress. */
bool fw_ota_lte_is_busy(void);

#ifdef __cplusplus
}
#endif
