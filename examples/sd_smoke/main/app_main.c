/*
 * SD card SPI smoke test for ESP32-C6 fleet board.
 *
 * Wiring (printed carrier — dedicated SPI2, not shared with MCP):
 *   SCK  GPIO4
 *   MOSI GPIO5
 *   MISO GPIO6
 *   CS   GPIO18
 *   VCC  5V or 3.3V per module; GND common
 *
 * MCP2515 CS (GPIO20) is held HIGH so the CAN chip stays quiet.
 *
 * Expect: mount OK, write/read /sdcard/smoke.txt, then PASS.
 */

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "sd_smoke";

#define PIN_SCK   4
#define PIN_MOSI  5
#define PIN_MISO  6
#define PIN_SD_CS 18
#define PIN_MCP_CS 20 /* hold high so MCP2515 does not drive MISO */

#define MOUNT_POINT "/sdcard"
#define TEST_PATH   MOUNT_POINT "/smoke.txt"
#define TEST_PAYLOAD "fleet-sd-smoke-ok\n"

static void hold_mcp_cs_high(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_MCP_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(PIN_MCP_CS, 1);
}

static esp_err_t mount_sd(sdmmc_card_t **out_card, bool format_if_needed)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_needed,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    /* Conservative clock for Dupont/soldered lab wiring */
    host.max_freq_khz = 4000;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = host.slot;

    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_sdspi_mount (format=%d): %s",
                 (int)format_if_needed, esp_err_to_name(err));
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "hint: card may be exFAT/unformatted — retry with format if enabled");
        } else if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_TIMEOUT ||
                   err == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "hint: check CS=%d SCK=%d MOSI=%d MISO=%d VCC GND; card seated?",
                     PIN_SD_CS, PIN_SCK, PIN_MOSI, PIN_MISO);
        }
        spi_bus_free(host.slot);
        return err;
    }

    *out_card = card;
    return ESP_OK;
}

static esp_err_t rw_test(void)
{
    FILE *f = fopen(TEST_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "fopen write failed for %s", TEST_PATH);
        return ESP_FAIL;
    }
    size_t n = fwrite(TEST_PAYLOAD, 1, strlen(TEST_PAYLOAD), f);
    fclose(f);
    if (n != strlen(TEST_PAYLOAD)) {
        ESP_LOGE(TAG, "fwrite short (%u)", (unsigned)n);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "wrote %s", TEST_PATH);

    f = fopen(TEST_PATH, "r");
    if (!f) {
        ESP_LOGE(TAG, "fopen read failed");
        return ESP_FAIL;
    }
    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        ESP_LOGE(TAG, "fgets failed");
        return ESP_FAIL;
    }
    fclose(f);

    if (strncmp(buf, "fleet-sd-smoke-ok", 17) != 0) {
        ESP_LOGE(TAG, "readback mismatch: '%s'", buf);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "readback OK: %s", buf);
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "SD SPI smoke  SCK=%d MOSI=%d MISO=%d CS=%d (MCP CS=%d held high)",
             PIN_SCK, PIN_MOSI, PIN_MISO, PIN_SD_CS, PIN_MCP_CS);

    hold_mcp_cs_high();

    sdmmc_card_t *card = NULL;
    for (;;) {
        /* First try existing FAT32; if that fails, format once (lab validation). */
        esp_err_t err = mount_sd(&card, false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mount without format failed — retrying WITH format (destroys card data)");
            err = mount_sd(&card, true);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MOUNT FAIL — likely wiring/CS/power (not just format). Retry in 5 s");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        sdmmc_card_print_info(stdout, card);

        err = rw_test();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "  SD SMOKE: PASS");
            ESP_LOGI(TAG, "========================================");
        } else {
            ESP_LOGE(TAG, "========================================");
            ESP_LOGE(TAG, "  SD SMOKE: FAIL (mount OK, R/W failed)");
            ESP_LOGE(TAG, "========================================");
        }

        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        spi_bus_free(SPI2_HOST);

        /* Stay idle so monitor keeps showing PASS; reboot to retest. */
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }
}
