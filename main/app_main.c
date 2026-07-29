#include "ble_elm.h"
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

static void format_addr(const uint8_t addr[6], char *out, size_t len)
{
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static void bonded_boot_init_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "bonded boot: waiting for BLE transport ready (up to %d ms)...",
             BONDED_BOOT_WAIT_MS);

    int waited_ms = 0;
    while (!elm327_client_is_ready() && waited_ms < BONDED_BOOT_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(BONDED_BOOT_POLL_MS));
        waited_ms += BONDED_BOOT_POLL_MS;
    }

    if (!elm327_client_is_ready()) {
        ESP_LOGW(TAG, "bonded boot: transport not ready after %d ms — clearing stale peer",
                 BONDED_BOOT_WAIT_MS);
        ble_elm_clear_peer();
        ble_bond_t empty;
        memset(&empty, 0, sizeof(empty));
        profile_store_set_bond(&empty);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "bonded boot: transport ready after %d ms", waited_ms);

    obd_profile_t profile;
    esp_err_t err = profile_store_get_active(&profile);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bonded boot: failed to load active profile: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    if (profile.init_at_count <= 0) {
        ESP_LOGI(TAG, "bonded boot: profile \"%s\" has no init sequence; skipping",
                 profile.name);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "bonded boot: running init sequence for profile \"%s\" (%d cmds)",
             profile.name, profile.init_at_count);
    err = elm327_client_run_init_sequence(profile.init_at, profile.init_at_count);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "bonded boot: init sequence OK");
        obd_poller_set_enabled(true);
        /* Kick protocol discovery with a long-timeout PID support query. */
        char resp[128];
        esp_err_t pid_err = elm327_client_transact("0100", resp, sizeof(resp), 15000);
        ESP_LOGI(TAG, "bonded boot: 0100 -> %s (%s)", resp, esp_err_to_name(pid_err));
    } else {
        ESP_LOGE(TAG, "bonded boot: init sequence failed: %s", esp_err_to_name(err));
        obd_poller_set_enabled(false);
    }

    vTaskDelete(NULL);
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

    ESP_ERROR_CHECK(ble_elm_init());
    ESP_LOGI(TAG, "ble_elm ready");

    ESP_ERROR_CHECK(elm327_client_init());
    ESP_LOGI(TAG, "elm327_client ready");

    ESP_ERROR_CHECK(transport_serial_start());
    ESP_LOGI(TAG, "transport_serial started");

    ESP_ERROR_CHECK(transport_http_start());
    ESP_LOGI(TAG, "transport_http started");

    ble_bond_t bond;
    if (profile_store_get_bond(&bond) == ESP_OK && bond.addr_set) {
        char addr_str[18];
        format_addr(bond.addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "bonded boot: connecting to saved device %s (%s)",
                 bond.name[0] != '\0' ? bond.name : "ELM327", addr_str);

        err = ble_elm_connect_addr(bond.addr);
        if (err == ESP_OK) {
            elm327_client_set_transport(ble_elm_get_transport());
            BaseType_t created = xTaskCreate(bonded_boot_init_task, "bonded_boot",
                                             BONDED_BOOT_TASK_STACK, NULL,
                                             BONDED_BOOT_TASK_PRIO, NULL);
            if (created != pdPASS) {
                ESP_LOGE(TAG, "bonded boot: failed to start init helper task");
            }
        } else {
            ESP_LOGE(TAG, "bonded boot: connect failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "bonded boot: no saved bond; use serial or HTTP to select a device");
    }

    ESP_ERROR_CHECK(obd_poller_start());
    ESP_LOGI(TAG, "obd_poller started");

    /* Auto-reconnect only after a live session exists. Enabling it against a
     * stale random address blocks BLE scan (EALREADY) and wastes airtime. */
    if (profile_store_get_bond(&bond) == ESP_OK && bond.addr_set &&
        ble_elm_is_connected()) {
        ESP_ERROR_CHECK(ble_elm_start_auto_reconnect());
        ESP_LOGI(TAG, "ble_elm auto-reconnect enabled");
    } else {
        ESP_LOGI(TAG, "ble_elm auto-reconnect deferred until select/connect succeeds");
    }
}
