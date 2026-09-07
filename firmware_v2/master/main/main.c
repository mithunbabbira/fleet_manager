#include "board_pins.h"
#include "can_obd.h"
#include "cli.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "lte.h"
#include "nvs_flash.h"
#include "obd_poller.h"
#include "ota_cloud.h"
#include "ota_flash.h"
#include "store_sd.h"
#include "transport_zigbee.h"
#include "uplink.h"

#include <stdio.h>

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "firmware_v2 master boot (nc-fleet-device; device_type obd/gps 1.0.32)");
    ESP_LOGI(TAG, "LTE pins TX=%d RX=%d (PCB frozen)", BOARD_LTE_TX_GPIO, BOARD_LTE_RX_GPIO);
    ESP_LOGI(TAG, "MCP soft-SPI SCK=%d MOSI=%d MISO=%d CS=%d INT=%d", BOARD_MCP_SCK_GPIO,
             BOARD_MCP_MOSI_GPIO, BOARD_MCP_MISO_GPIO, BOARD_MCP_CS_GPIO, BOARD_MCP_INT_GPIO);
    ESP_LOGI(TAG, "SD SPI2 SCK=%d MOSI=%d MISO=%d CS=%d", BOARD_SD_SCK_GPIO, BOARD_SD_MOSI_GPIO,
             BOARD_SD_MISO_GPIO, BOARD_SD_CS_GPIO);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    (void)store_sd_spi_lock_init();
    store_sd_spi_cs_idle_high();

    ESP_ERROR_CHECK(ota_flash_init());
    ESP_ERROR_CHECK(ota_cloud_init());

    bool marked = false;
    if (ota_flash_confirm_after_boot(&marked) == ESP_OK && marked) {
        (void)ota_cloud_commit_pending_applied();
        ESP_LOGI(TAG, "OTA image confirmed valid");
    }

    ota_flash_status_t st;
    ota_flash_get_status(&st);
    printf("=== firmware_v2 master ===\n");
    printf("version=%s partition=%s\n", st.fw_version, st.running_partition);
    printf("lte_tx_gpio=%d lte_rx_gpio=%d\n", BOARD_LTE_TX_GPIO, BOARD_LTE_RX_GPIO);
    printf("mcp_cs_gpio=%d sd_cs_gpio=%d\n", BOARD_MCP_CS_GPIO, BOARD_SD_CS_GPIO);

    err = lte_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lte_start: %s", esp_err_to_name(err));
    }

    (void)ota_cloud_start_auto();

    err = can_obd_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can_obd_init: %s", esp_err_to_name(err));
    } else {
        (void)can_obd_start();
    }

    err = obd_poller_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "obd_poller_init: %s", esp_err_to_name(err));
    } else {
        (void)obd_poller_start();
    }

    err = store_sd_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "store_sd_init: %s (continuing without SD queue)", esp_err_to_name(err));
    }

    err = transport_zigbee_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "transport_zigbee_init: %s", esp_err_to_name(err));
    } else {
        err = transport_zigbee_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "transport_zigbee_start: %s", esp_err_to_name(err));
        }
    }

    ESP_ERROR_CHECK(uplink_init());
    (void)uplink_start();

    (void)cli_start();
}
