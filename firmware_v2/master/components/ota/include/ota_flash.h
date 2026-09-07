#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_FLASH_STATE_IDLE = 0,
    OTA_FLASH_STATE_WRITING,
    OTA_FLASH_STATE_FAILED,
    OTA_FLASH_STATE_PENDING_REBOOT,
} ota_flash_state_t;

typedef struct {
    ota_flash_state_t state;
    char error[96];
    char running_partition[24];
    char update_partition[24];
    char fw_version[32];
    size_t expected_size;
    size_t bytes_written;
    bool pending_verify;
} ota_flash_status_t;

/**
 * @brief Create the OTA mutex and reset status to IDLE.
 * @note Call once from app_main after NVS. Shared by SoftAP HTTP and ota_flash_lte.
 * @return ESP_OK, or ESP_ERR_NO_MEM if the mutex cannot be created.
 */
esp_err_t ota_flash_init(void);

/**
 * If the running image is ESP_OTA_IMG_PENDING_VERIFY, mark it valid.
 * Call after SoftAP/HTTP is up (lab health gate).
 *
 * @param marked_valid Optional; set true only when this call cancelled rollback.
 *                     Pass NULL if unused. Callers should promote ota_pend →
 *                     ota_applied via ota_flash_lte_commit_pending_applied() when true.
 */
esp_err_t ota_flash_confirm_after_boot(bool *marked_valid);

/**
 * @brief Open the inactive app slot and start a streamed OTA write.
 * @param expected_size Exact image length in bytes (must fit the update partition).
 * @param sha256_hex 64-char hex digest from the manifest (validated before begin).
 * @note Mutex-serialized; rejects concurrent WRITING or missing ota_flash_init().
 * @return ESP_OK, or INVALID_ARG/STATE/SIZE/NOT_FOUND / esp_ota_begin error.
 */
esp_err_t ota_flash_begin(size_t expected_size, const char *sha256_hex);

/**
 * @brief Append a chunk to the active session and update the running SHA-256.
 * @param data Chunk bytes (may be NULL only if @p len is 0).
 * @param len Chunk length; overflow vs expected_size aborts the session.
 * @note On flash/write failure, aborts and leaves state FAILED.
 * @return ESP_OK, INVALID_ARG/STATE/SIZE, or esp_ota_write error.
 */
esp_err_t ota_flash_write(const void *data, size_t len);

/**
 * @brief Cancel an in-progress write and release the esp_ota handle.
 * @note WRITING → FAILED (preserves prior error string if set). Safe when idle.
 * @return ESP_OK, or ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t ota_flash_abort(void);

/**
 * @brief Verify size+SHA-256, finalize OTA, set boot partition, then esp_restart().
 * @note Does not return on success. Size/hash mismatch aborts and returns an error.
 * @return Error code on failure; ESP_OK only in the unreachable success path.
 */
esp_err_t ota_flash_end_and_reboot(void);

/**
 * @brief Snapshot progress, partitions, fw version, and pending-verify flag.
 * @param out Filled status (required).
 * @note Takes the OTA mutex when present; safe for SoftAP status polls.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if @p out is NULL.
 */
esp_err_t ota_flash_get_status(ota_flash_status_t *out);

/**
 * @brief True while WRITING or PENDING_REBOOT (block concurrent SoftAP/LTE OTAs).
 */
bool ota_flash_is_busy(void);

#ifdef __cplusplus
}
#endif
