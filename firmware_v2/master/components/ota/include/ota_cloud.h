#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_CLOUD_IDLE = 0,
    OTA_CLOUD_CHECKING,
    OTA_CLOUD_DOWNLOADING,
    OTA_CLOUD_NO_UPDATE,
    OTA_CLOUD_FAILED,
    OTA_CLOUD_REBOOTING,
} ota_cloud_phase_t;

typedef struct {
    char manifest_url[256]; /* firmware-check POST URL */
    char channel[24];
    char device_id[40];
    char auth_token[192];   /* Authorization header value; empty = omit */
    char system_user_id[32]; /* x-nc-system-user-id; empty = omit */
    bool force;
} ota_cloud_config_t;

typedef struct {
    ota_cloud_phase_t phase;
    char error[96];
    char manifest_version[40]; /* latestVersion from check response */
    char applied_version[40];
    char current_version[24];  /* stripped value sent in POST */
    bool update_available;
    int http_status;
    size_t bytes_downloaded;
} ota_cloud_status_t;

/**
 * @brief Create OTA mutex; load NVS config and ota_applied; phase IDLE.
 * @note Call once before other APIs (no NULL guard on s_mu elsewhere).
 */
esp_err_t ota_cloud_init(void);
esp_err_t ota_cloud_get_config(ota_cloud_config_t *out);
/**
 * @brief Replace config, sanitize legacy lab URLs, persist (incl. uplink_did).
 * @note Does not refresh telemetry_uplink's in-RAM device_id.
 */
esp_err_t ota_cloud_set_config(const ota_cloud_config_t *in);
esp_err_t ota_cloud_get_status(ota_cloud_status_t *out);

/**
 * @brief Blocking Trafyn check → optional stream/flash/reboot.
 * @warning Does NOT suspend BG AT; prefer start_background.
 * @note On successful download, writes ota_pend (not ota_applied). Promote via
 *       ota_cloud_commit_pending_applied() after ota_flash_confirm_after_boot marks valid.
 *       Does not return on successful reboot.
 */
esp_err_t ota_cloud_run(void);

/**
 * @brief Promote NVS ota_pend → ota_applied after a successful boot verify.
 * @note Call when ota_flash_confirm_after_boot sets marked_valid. Clears ota_pend.
 *       No-op if no pending version is stored.
 */
esp_err_t ota_cloud_commit_pending_applied(void);

/**
 * @brief Spawn worker: suspend_bg_at → run → resume. Rejects if s_task or fw_ota busy.
 */
esp_err_t ota_cloud_start_background(void);

/**
 * @brief Start periodic auto-check task (wait LTE → background OTA → sleep hours).
 * @note No-op / ESP_ERR_NOT_SUPPORTED when CONFIG_OTA_CLOUD_AUTO_CHECK is unset.
 *       Auto path always uses force=false.
 */
esp_err_t ota_cloud_start_auto(void);

/**
 * @brief True if phase CHECKING/DOWNLOADING/REBOOTING or worker task alive.
 */
bool ota_cloud_is_busy(void);

#ifdef __cplusplus
}
#endif
