#include "board_pins.h"
#include "cli.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "lte.h"
#include "nvs_flash.h"
#include "ota_cloud.h"
#include "ota_flash.h"

#include <stdio.h>

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "firmware_v2 master boot (lab OTA target 1.0.12)");
    ESP_LOGI(TAG, "LTE pins TX=%d RX=%d (PCB frozen)", BOARD_LTE_TX_GPIO, BOARD_LTE_RX_GPIO);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

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

    err = lte_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lte_start: %s", esp_err_to_name(err));
    }

    (void)ota_cloud_start_auto();
    (void)cli_start();
}
