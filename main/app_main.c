#include "can_obd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_ota.h"
#include "fw_ota_lte.h"
#include "net_lte.h"
#include "nvs_flash.h"
#include "obd_poller.h"
#include "profile_store.h"
#include "store_sd.h"
#include "sys_runtime.h"
#include "telemetry_bus.h"
#include "sdkconfig.h"
#include "transport_http.h"
#include "transport_serial.h"
#include "transport_zigbee.h"
#include "telemetry_uplink.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app";

#define BONDED_BOOT_TASK_STACK 4096
#define BONDED_BOOT_TASK_PRIO  5
#define BONDED_BOOT_WAIT_MS    15000
#define BONDED_BOOT_POLL_MS    200

/**
 * @brief CAN link supervisor: enable obd_poller when ECU answers; pause on link loss.
 * @note Historical task name "bonded_boot"; BLE path removed. Polls can_obd_is_ready.
 */
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

/**
 * @brief Fleet telematics entry from the active OTA slot (ota_0 or ota_1).
 * @note Boot order: NVS → fw_ota* → profiles/bus → LTE+auto OTA → SPI/CAN → SD →
 *       uplink → serial/SoftAP → OTA confirm → poller → can_boot. Soft failures
 *       (LTE/SD/SoftAP) continue; NVS/sys_runtime/serial/poller are hard-checked.
 *
 *  1) NVS          — settings survive OTA (Trafyn host, uplink, profiles, …)
 *  2) fw_ota*      — dual-bank flash writer + LTE OTA client (loads ota_manif)
 *  3) profiles/bus — OBD poll config + in-process telemetry pub/sub
 *  4) LTE + auto   — EC200U UART; then background Trafyn firmware-check POST
 *  5) SPI + CAN    — MCP soft-SPI (separate GPIOs from SD SPI2)
 *  6) store_sd     — microSD mount on hardware SPI2 (CS GPIO18)
 *  7) uplink       — produce→SD queue, drain→batch HTTPS POST
 *  8) serial       — USB console (PC Carrier Console over serial)
 *  9) OTA confirm  — mark pending image valid after console is up
 * 10) poller       — OBD PID polling (enabled by can_boot_task when link up)
 *
 * LTE auto-check (fw_ota_lte_start_auto): wait for registration → POST check →
 * compare version → stream .bin into inactive ota_X → update otadata → reboot.
 * SoftAP/serial can still trigger the same path manually.
 */
void app_main(void)
{
    /* --- NVS partition (elm namespace keys: ota_manif, uplink_*, profiles, …) --- */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "ESP32-C6 fleet telematics node boot");

    ESP_ERROR_CHECK(sys_runtime_init());
    ESP_LOGI(TAG, "sys_runtime ready");

    /* Dual-bank writer (ota_0/ota_1) + LTE orchestrator (Trafyn check / download). */
    ESP_ERROR_CHECK(fw_ota_init());
    ESP_LOGI(TAG, "fw_ota ready");
    ESP_ERROR_CHECK(fw_ota_lte_init());
    ESP_LOGI(TAG, "fw_ota_lte ready");

    ESP_ERROR_CHECK(profile_store_init());
    ESP_LOGI(TAG, "profile_store ready");
    obd_profile_t active;
    if (profile_store_get_active(&active) == ESP_OK) {
        ESP_LOGI(TAG, "active profile: %s (%d items)", active.name, active.item_count);
    }

    ESP_ERROR_CHECK(telemetry_bus_init());
    ESP_LOGI(TAG, "telemetry_bus ready");

    ESP_ERROR_CHECK(transport_zigbee_init());
    ESP_ERROR_CHECK(transport_zigbee_start());
    ESP_LOGI(TAG, "transport_zigbee ready");

    /*
     * LTE (EC200U on UART1: GPIO16 TX / GPIO17 RX).
     * Bring-up is background; once AT works, start OTA auto-check task
     * (waits ~90s for registration, then Trafyn firmware-check POST every 24h).
     */
    {
        esp_err_t lte_err = net_lte_start();
        if (lte_err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(TAG, "net_lte: disabled (CONFIG_NET_LTE_ENABLE=n)");
        } else if (lte_err != ESP_OK) {
            ESP_LOGW(TAG, "net_lte_start: %s (continuing without LTE)",
                     esp_err_to_name(lte_err));
        } else {
            ESP_LOGI(TAG, "net_lte ready");
            esp_err_t auto_err = fw_ota_lte_start_auto();
            if (auto_err == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGI(TAG, "fw_ota_lte auto-check disabled");
            } else if (auto_err != ESP_OK) {
                ESP_LOGW(TAG, "fw_ota_lte_start_auto: %s", esp_err_to_name(auto_err));
            } else {
                ESP_LOGI(TAG, "fw_ota_lte auto-check started");
            }
        }
    }

    /* SPI CS idle + MCP soft-SPI / SD hardware-SPI (separate wires).
     * ESP32-C6 has one GPSPI: SD uses SPI2 on GPIO4/5/6/18; MCP bitbangs 21/22/23/20. */
    ESP_ERROR_CHECK(store_sd_spi_lock_init());
    store_sd_spi_cs_idle_high();

    /* MCP2515 soft-SPI CAN → ISO-TP OBD. */
    err = can_obd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "can_obd_init: %s — check MCP2515 wiring/power",
                 esp_err_to_name(err));
    } else {
        ESP_ERROR_CHECK(can_obd_start());
        ESP_LOGI(TAG, "can_obd started (protocol autodetect)");
    }

    /* microSD on shared SPI2 (CS GPIO18). Non-fatal if missing. */
    {
        esp_err_t sd_err = store_sd_init();
        if (sd_err == ESP_OK) {
            ESP_LOGI(TAG, "store_sd ready");
        } else if (sd_err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(TAG, "store_sd disabled");
        } else {
            ESP_LOGW(TAG, "store_sd_init: %s (uplink will use live POST)",
                     esp_err_to_name(sd_err));
        }
    }

    /* Cloud telemetry: enqueue to SD + batch drain (or live POST without SD). */
    {
        esp_err_t up_err = telemetry_uplink_start();
        if (up_err != ESP_OK) {
            ESP_LOGW(TAG, "telemetry_uplink_start: %s (continuing)",
                     esp_err_to_name(up_err));
        } else {
            ESP_LOGI(TAG, "telemetry_uplink ready");
        }
    }

    /* USB console: config, ota, lte, uplink, fleet, … — PC Carrier Console on serial. */
    ESP_ERROR_CHECK(transport_serial_start());
    ESP_LOGI(TAG, "transport_serial started");

#if CONFIG_ELM_HTTP_ENABLE
    /* Optional legacy SoftAP + embedded HTTP (default off — use Carrier Console). */
    {
        esp_err_t http_err = transport_http_start();
        if (http_err != ESP_OK) {
            ESP_LOGW(TAG, "transport_http_start: %s", esp_err_to_name(http_err));
        } else {
            ESP_LOGI(TAG, "transport_http started");
        }
    }
#endif

    /*
     * If this boot is a new OTA image in PENDING_VERIFY, mark it valid and cancel
     * rollback (otadata). Runs after serial console is up.
     */
    {
        bool marked = false;
        esp_err_t ota_err = fw_ota_confirm_after_boot(&marked);
        if (ota_err != ESP_OK) {
            ESP_LOGW(TAG, "fw_ota_confirm_after_boot: %s", esp_err_to_name(ota_err));
        } else if (marked) {
            esp_err_t c_err = fw_ota_lte_commit_pending_applied();
            if (c_err != ESP_OK) {
                ESP_LOGW(TAG, "fw_ota_lte_commit_pending_applied: %s",
                         esp_err_to_name(c_err));
            }
        }
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
