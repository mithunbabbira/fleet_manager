#include "ble_elm.h"
#include "can_obd.h"
#include "elm327_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_lte.h"
#include "nvs_flash.h"
#include "obd_poller.h"
#include "profile_store.h"
#include "sys_runtime.h"
#include "telemetry_bus.h"
#include "transport_http.h"
#include "transport_serial.h"
#include "telemetry_uplink.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app";

#define BONDED_BOOT_TASK_STACK 4096
#define BONDED_BOOT_TASK_PRIO  5
#define BONDED_BOOT_WAIT_MS    15000
#define BONDED_BOOT_POLL_MS    200

/* Enable profile polling as soon as the CAN link finds an ECU; pause it on
 * link loss so can_obd can reprobe protocols without poll traffic. */
static void can_boot_task(void *arg)
{
    (void)arg;
    bool was_ready = false;

    for (;;) {
        bool ready = can_obd_is_ready();
        if (ready && !was_ready) {
            char proto[48];
            can_obd_get_protocol(proto, sizeof(proto));
            ESP_LOGI(TAG, "CAN link up (%s); enabling poller", proto);
            obd_poller_set_enabled(true);
        } else if (!ready && was_ready) {
            ESP_LOGW(TAG, "CAN link down; pausing poller");
            obd_poller_set_enabled(false);
        }
        was_ready = ready;
        vTaskDelay(pdMS_TO_TICKS(BONDED_BOOT_POLL_MS));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "ELM327 ESP32-C6 bridge boot");

    ESP_ERROR_CHECK(sys_runtime_init());
    ESP_LOGI(TAG, "sys_runtime ready");

    ESP_ERROR_CHECK(profile_store_init());
    ESP_LOGI(TAG, "profile_store ready");
    obd_profile_t active;
    if (profile_store_get_active(&active) == ESP_OK) {
        ESP_LOGI(TAG, "active profile: %s (%d items)", active.name, active.item_count);
    }

    ESP_ERROR_CHECK(telemetry_bus_init());
    ESP_LOGI(TAG, "telemetry_bus ready");

    /* UART AT check / PPP: requires EC200U wired to GPIO17 TX / GPIO16 RX. */
    {
        esp_err_t lte_err = net_lte_start();
        if (lte_err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(TAG, "net_lte: disabled (CONFIG_NET_LTE_ENABLE=n)");
        } else if (lte_err != ESP_OK) {
            ESP_LOGW(TAG, "net_lte_start: %s (continuing without LTE)",
                     esp_err_to_name(lte_err));
        } else {
            ESP_LOGI(TAG, "net_lte ready");
        }
    }

    {
        esp_err_t up_err = telemetry_uplink_start();
        if (up_err != ESP_OK) {
            ESP_LOGW(TAG, "telemetry_uplink_start: %s (continuing)",
                     esp_err_to_name(up_err));
        } else {
            ESP_LOGI(TAG, "telemetry_uplink ready");
        }
    }

    /* BLE/ELM stay initialized (serial + HTTP transports reference them) but
     * this branch never connects: OBD data comes from the MCP2515 CAN link. */
    ESP_ERROR_CHECK(ble_elm_init());
    ESP_LOGI(TAG, "ble_elm ready (idle on CAN branch)");

    ESP_ERROR_CHECK(elm327_client_init());
    ESP_LOGI(TAG, "elm327_client ready (idle on CAN branch)");

    ESP_ERROR_CHECK(transport_serial_start());
    ESP_LOGI(TAG, "transport_serial started");

    ESP_ERROR_CHECK(transport_http_start());
    ESP_LOGI(TAG, "transport_http started");

    err = can_obd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "can_obd_init: %s — check MCP2515 wiring/power",
                 esp_err_to_name(err));
    } else {
        ESP_ERROR_CHECK(can_obd_start());
        ESP_LOGI(TAG, "can_obd started (protocol autodetect)");
    }

    ESP_ERROR_CHECK(obd_poller_start());
    ESP_LOGI(TAG, "obd_poller started");

    if (err == ESP_OK) {
        BaseType_t created = xTaskCreate(can_boot_task, "can_boot",
                                         BONDED_BOOT_TASK_STACK, NULL,
                                         BONDED_BOOT_TASK_PRIO, NULL);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "failed to start can_boot task");
        }
    }
}
