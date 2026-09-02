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
    char manifest_url[256]; /* firmware-check POST URL (NVS/UI name kept) */
    char channel[24];
    char device_id[40];
    bool force;
} fw_ota_lte_config_t;

typedef struct {
    fw_ota_lte_phase_t phase;
    char error[96];
    char manifest_version[40]; /* latestVersion from check response */
    char applied_version[40];
    char current_version[24];  /* stripped value sent in POST */
    bool update_available;
    int http_status;
    size_t bytes_downloaded;
} fw_ota_lte_status_t;

/**
 * @brief Create OTA mutex; load NVS config and ota_applied; phase IDLE.
 * @note Call once before other APIs (no NULL guard on s_mu elsewhere).
 */
esp_err_t fw_ota_lte_init(void);
esp_err_t fw_ota_lte_get_config(fw_ota_lte_config_t *out);
/**
 * @brief Replace config, sanitize legacy lab URLs, persist (incl. uplink_did).
 * @note Does not refresh telemetry_uplink's in-RAM device_id.
 */
esp_err_t fw_ota_lte_set_config(const fw_ota_lte_config_t *in);
esp_err_t fw_ota_lte_get_status(fw_ota_lte_status_t *out);

/**
 * @brief Blocking Trafyn check → optional stream/flash/reboot.
 * @warning Does NOT suspend BG AT; prefer start_background.
 * @note On successful download, writes ota_pend (not ota_applied). Promote via
 *       fw_ota_lte_commit_pending_applied() after fw_ota_confirm_after_boot marks valid.
 *       Does not return on successful reboot.
 */
esp_err_t fw_ota_lte_run(void);

/**
 * @brief Promote NVS ota_pend → ota_applied after a successful boot verify.
 * @note Call when fw_ota_confirm_after_boot sets marked_valid. Clears ota_pend.
 *       No-op if no pending version is stored.
 */
esp_err_t fw_ota_lte_commit_pending_applied(void);

/**
 * @brief Spawn worker: suspend_bg_at → run → resume. Rejects if s_task or fw_ota busy.
 */
esp_err_t fw_ota_lte_start_background(void);

/**
 * @brief Start periodic auto-check task (wait LTE → background OTA → sleep hours).
 * @note No-op / ESP_ERR_NOT_SUPPORTED when CONFIG_FW_OTA_LTE_AUTO_CHECK is unset.
 *       Auto path always uses force=false.
 */
esp_err_t fw_ota_lte_start_auto(void);

/**
 * @brief True if phase CHECKING/DOWNLOADING/REBOOTING or worker task alive.
 */
bool fw_ota_lte_is_busy(void);

#ifdef __cplusplus
}
#endif
