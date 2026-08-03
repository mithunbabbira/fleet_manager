#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FW_OTA_STATE_IDLE = 0,
    FW_OTA_STATE_WRITING,
    FW_OTA_STATE_FAILED,
    FW_OTA_STATE_PENDING_REBOOT,
} fw_ota_state_t;

typedef struct {
    fw_ota_state_t state;
    char error[96];
    char running_partition[24];
    char update_partition[24];
    char fw_version[32];
    size_t expected_size;
    size_t bytes_written;
    bool pending_verify;
} fw_ota_status_t;

/** Initialize mutex / status. Call once from app_main after NVS. */
esp_err_t fw_ota_init(void);

/**
 * If the running image is ESP_OTA_IMG_PENDING_VERIFY, mark it valid.
 * Call after SoftAP/HTTP is up (lab health gate).
 */
esp_err_t fw_ota_confirm_after_boot(void);

esp_err_t fw_ota_begin(size_t expected_size, const char *sha256_hex);
esp_err_t fw_ota_write(const void *data, size_t len);
esp_err_t fw_ota_abort(void);

/** Verify size+hash, finalize OTA, set boot partition, then esp_restart(). Does not return on success. */
esp_err_t fw_ota_end_and_reboot(void);

esp_err_t fw_ota_get_status(fw_ota_status_t *out);

/** True while an OTA write/reboot is in progress (SoftAP or LTE). */
bool fw_ota_is_busy(void);

#ifdef __cplusplus
}
#endif
