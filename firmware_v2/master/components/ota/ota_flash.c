#include "ota_flash.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "mbedtls/sha256.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ota_flash";

#define SHA256_HEX_LEN 64
#define SHA256_BIN_LEN 32

static SemaphoreHandle_t s_mu;
static ota_flash_state_t s_state = OTA_FLASH_STATE_IDLE;
static char s_error[96];
static esp_ota_handle_t s_ota;
static const esp_partition_t *s_update;
static size_t s_expected;
static size_t s_written;
static uint8_t s_expect_sha[SHA256_BIN_LEN];
static mbedtls_sha256_context s_sha;
static bool s_sha_active;

/*
 * fw_ota is the generic "write an inactive OTA app slot" helper.
 *
 * Key points:
 * - The partition table provides two app slots: ota_0 and ota_1.
 * - During an update, we write into the *inactive* slot (next update partition).
 * - After streaming the image, we:
 *    1) verify sha256 matches the manifest
 *    2) call esp_ota_end()
 *    3) set boot partition to the newly written slot (esp_ota_set_boot_partition)
 *    4) reboot (esp_restart)
 *
 * The actual boot choice is persisted in the `otadata` partition by ESP-IDF.
 */

/**
 * @brief Store a short human-readable failure reason for status queries.
 */
static void set_error(const char *msg)
{
    snprintf(s_error, sizeof(s_error), "%s", msg ? msg : "error");
}

/**
 * @brief Clear the last error string (successful begin / init).
 */
static void clear_error(void)
{
    s_error[0] = '\0';
}

/**
 * @brief Parse a 64-char hex SHA-256 into 32 binary bytes.
 * @return false if length or hex digits are invalid.
 */
static bool parse_sha256_hex(const char *hex, uint8_t out[SHA256_BIN_LEN])
{
    if (!hex || strlen(hex) != SHA256_HEX_LEN) {
        return false;
    }
    for (int i = 0; i < SHA256_BIN_LEN; ++i) {
        char a = hex[i * 2];
        char b = hex[i * 2 + 1];
        if (!isxdigit((unsigned char)a) || !isxdigit((unsigned char)b)) {
            return false;
        }
        char pair[3] = {a, b, '\0'};
        out[i] = (uint8_t)strtoul(pair, NULL, 16);
    }
    return true;
}

/**
 * @brief Map ota_flash_state_t to a stable log/status string.
 */
static const char *state_str(ota_flash_state_t st)
{
    switch (st) {
    case OTA_FLASH_STATE_IDLE: return "idle";
    case OTA_FLASH_STATE_WRITING: return "writing";
    case OTA_FLASH_STATE_FAILED: return "failed";
    case OTA_FLASH_STATE_PENDING_REBOOT: return "pending_reboot";
    default: return "unknown";
    }
}

/**
 * @brief Create mutex (once) and force IDLE with a cleared error.
 * @return ESP_OK, or ESP_ERR_NO_MEM if mutex create fails.
 */
esp_err_t ota_flash_init(void)
{
    if (s_mu == NULL) {
        s_mu = xSemaphoreCreateMutex();
        if (s_mu == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    clear_error();
    s_state = OTA_FLASH_STATE_IDLE;
    ESP_LOGI(TAG, "init (state=%s)", state_str(s_state));
    return ESP_OK;
}

esp_err_t ota_flash_confirm_after_boot(bool *marked_valid)
{
    /*
     * Lab health gate:
     * If the running slot is ESP_OTA_IMG_PENDING_VERIFY, mark it valid to cancel
     * rollback (so the device stays on the new image).
     *
     * This runs after SoftAP/HTTP comes up (see main/app_main.c).
     */
    if (marked_valid) {
        *marked_valid = false;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_img_states_t img_state;
    esp_err_t err = esp_ota_get_state_partition(running, &img_state);
    if (err != ESP_OK) {
        /* Rollback not enabled or partition has no otadata state — not fatal. */
        ESP_LOGW(TAG, "esp_ota_get_state_partition: %s", esp_err_to_name(err));
        return ESP_OK;
    }

    if (img_state != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "running %s not pending verify (state=%d)", running->label, (int)img_state);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "pending verify on %s — marking valid (lab SoftAP health gate)", running->label);
    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mark_app_valid failed: %s", esp_err_to_name(err));
        return err;
    }
    if (marked_valid) {
        *marked_valid = true;
    }
    return ESP_OK;
}

/**
 * @brief Begin dual-bank write: parse SHA, pick next slot, esp_ota_begin + SHA context.
 * @note Sets FAILED and returns on bad SHA, missing partition, oversize, or begin error.
 * @return ESP_OK when state becomes WRITING.
 */
esp_err_t ota_flash_begin(size_t expected_size, const char *sha256_hex)
{
    if (!s_mu) {
        return ESP_ERR_INVALID_STATE;
    }
    if (expected_size == 0 || !sha256_hex) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_state == OTA_FLASH_STATE_WRITING) {
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_STATE;
    }

    clear_error();
    if (!parse_sha256_hex(sha256_hex, s_expect_sha)) {
        set_error("invalid_sha256");
        s_state = OTA_FLASH_STATE_FAILED;
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_ARG;
    }

    s_update = esp_ota_get_next_update_partition(NULL);
    if (!s_update) {
        set_error("no_update_partition");
        s_state = OTA_FLASH_STATE_FAILED;
        xSemaphoreGive(s_mu);
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_size > s_update->size) {
        set_error("image_too_large");
        s_state = OTA_FLASH_STATE_FAILED;
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_begin(s_update, expected_size, &s_ota);
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "ota_begin:%s", esp_err_to_name(err));
        s_state = OTA_FLASH_STATE_FAILED;
        xSemaphoreGive(s_mu);
        return err;
    }

    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);
    s_sha_active = true;
    s_expected = expected_size;
    s_written = 0;
    s_state = OTA_FLASH_STATE_WRITING;
    ESP_LOGI(TAG, "begin write → %s size=%u", s_update->label, (unsigned)expected_size);
    xSemaphoreGive(s_mu);
    return ESP_OK;
}

/**
 * @brief Stream one chunk under mutex; update SHA; abort on overflow or flash error.
 * @note May call ota_flash_abort() after releasing the mutex on failure paths.
 */
esp_err_t ota_flash_write(const void *data, size_t len)
{
    if (!data && len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mu) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_state != OTA_FLASH_STATE_WRITING) {
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_written + len > s_expected) {
        set_error("overflow");
        s_state = OTA_FLASH_STATE_FAILED;
        xSemaphoreGive(s_mu);
        ota_flash_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ESP_OK;
    if (len > 0) {
        err = esp_ota_write(s_ota, data, len);
        if (err == ESP_OK && s_sha_active) {
            mbedtls_sha256_update(&s_sha, data, len);
        }
    }
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "ota_write:%s", esp_err_to_name(err));
        s_state = OTA_FLASH_STATE_FAILED;
        xSemaphoreGive(s_mu);
        ota_flash_abort();
        return err;
    }
    s_written += len;
    xSemaphoreGive(s_mu);
    return ESP_OK;
}

/**
 * @brief Tear down SHA/OTA handles and mark FAILED if a write was active.
 */
esp_err_t ota_flash_abort(void)
{
    if (!s_mu) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_sha_active) {
        mbedtls_sha256_free(&s_sha);
        s_sha_active = false;
    }
    if (s_ota) {
        esp_ota_abort(s_ota);
        s_ota = 0;
    }
    s_update = NULL;
    s_expected = 0;
    s_written = 0;
    if (s_state == OTA_FLASH_STATE_WRITING) {
        s_state = OTA_FLASH_STATE_FAILED;
        if (s_error[0] == '\0') {
            set_error("aborted");
        }
    }
    xSemaphoreGive(s_mu);
    ESP_LOGW(TAG, "aborted (%s)", s_error);
    return ESP_OK;
}

/**
 * @brief Finalize write: exact size, SHA match, esp_ota_end, set boot, reboot.
 * @note Success path calls esp_restart() and does not return; failures abort/return.
 */
esp_err_t ota_flash_end_and_reboot(void)
{
    if (!s_mu) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_state != OTA_FLASH_STATE_WRITING) {
        xSemaphoreGive(s_mu);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_written != s_expected) {
        set_error("size_mismatch");
        s_state = OTA_FLASH_STATE_FAILED;
        xSemaphoreGive(s_mu);
        ota_flash_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t got[SHA256_BIN_LEN];
    mbedtls_sha256_finish(&s_sha, got);
    mbedtls_sha256_free(&s_sha);
    s_sha_active = false;

    if (memcmp(got, s_expect_sha, SHA256_BIN_LEN) != 0) {
        set_error("sha256_mismatch");
        s_state = OTA_FLASH_STATE_FAILED;
        xSemaphoreGive(s_mu);
        ota_flash_abort();
        return ESP_ERR_INVALID_CRC;
    }

    esp_err_t err = esp_ota_end(s_ota);
    s_ota = 0;
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "ota_end:%s", esp_err_to_name(err));
        s_state = OTA_FLASH_STATE_FAILED;
        xSemaphoreGive(s_mu);
        return err;
    }

    err = esp_ota_set_boot_partition(s_update);
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "set_boot:%s", esp_err_to_name(err));
        s_state = OTA_FLASH_STATE_FAILED;
        xSemaphoreGive(s_mu);
        return err;
    }

    s_state = OTA_FLASH_STATE_PENDING_REBOOT;
    ESP_LOGI(TAG, "OTA OK — rebooting into %s", s_update->label);
    xSemaphoreGive(s_mu);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK; /* not reached */
}

/**
 * @brief Fill @p out with mutex-protected session fields plus running-partition info.
 */
esp_err_t ota_flash_get_status(ota_flash_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
    out->state = s_state;
    snprintf(out->error, sizeof(out->error), "%s", s_error);
    out->expected_size = s_expected;
    out->bytes_written = s_written;

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        snprintf(out->running_partition, sizeof(out->running_partition), "%s", running->label);
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
            out->pending_verify = (st == ESP_OTA_IMG_PENDING_VERIFY);
        }
    }
    if (s_update) {
        snprintf(out->update_partition, sizeof(out->update_partition), "%s", s_update->label);
    } else {
        const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
        if (next) {
            snprintf(out->update_partition, sizeof(out->update_partition), "%s", next->label);
        }
    }
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        snprintf(out->fw_version, sizeof(out->fw_version), "%s", app->version);
    }
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
    return ESP_OK;
}

/**
 * @brief Convenience busy check for SoftAP/LTE to refuse overlapping updates.
 */
bool ota_flash_is_busy(void)
{
    ota_flash_status_t st;
    if (ota_flash_get_status(&st) != ESP_OK) {
        return false;
    }
    return st.state == OTA_FLASH_STATE_WRITING || st.state == OTA_FLASH_STATE_PENDING_REBOOT;
}
