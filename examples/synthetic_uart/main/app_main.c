#include "synthetic_data.h"

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLEET_UART UART_NUM_1
#define FLEET_TX_GPIO 17
#define FLEET_RX_GPIO 16
#define FLEET_BAUD 9600
#define RX_BUF_SIZE 512

static const char *TAG = "synthetic_uart";
static uint32_t s_cmds_ok;
static uint32_t s_cmds_fail;

static cJSON *add_sample(cJSON *samples, const char *key, const char *cmd,
                         double value, const char *unit, const char *raw)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return NULL;
    }

    if (!cJSON_AddStringToObject(item, "k", key) ||
        !cJSON_AddStringToObject(item, "cmd", cmd) ||
        !cJSON_AddNumberToObject(item, "v", value) ||
        !cJSON_AddStringToObject(item, "u", unit) ||
        !cJSON_AddStringToObject(item, "raw", raw) ||
        !cJSON_AddBoolToObject(item, "ok", true) ||
        !cJSON_AddNumberToObject(item, "age_ms", 0) ||
        !cJSON_AddItemToArray(samples, item)) {
        cJSON_Delete(item);
        return NULL;
    }

    return item;
}

static char *build_snapshot_json(const synthetic_snapshot_t *s, uint32_t cmds_ok)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *link = cJSON_CreateObject();
    cJSON *metrics = cJSON_CreateObject();
    cJSON *samples = cJSON_CreateArray();
    char *text = NULL;

    if (!root || !link || !metrics || !samples) {
        goto cleanup;
    }

    if (!cJSON_AddNumberToObject(root, "v", 1) ||
        !cJSON_AddStringToObject(root, "node_id", "esp32c6-01") ||
        !cJSON_AddStringToObject(root, "vehicle_id", "fleet-demo-001") ||
        !cJSON_AddStringToObject(root, "ble_peer", "46:FC:0D:32:1E:66") ||
        !cJSON_AddStringToObject(root, "adapter", "MODAXE OBDII") ||
        !cJSON_AddStringToObject(root, "profile", "can_11_500") ||
        !cJSON_AddStringToObject(root, "protocol", "ISO15765-4 CAN11/500") ||
        !cJSON_AddNumberToObject(root, "seq", s->sequence) ||
        !cJSON_AddNumberToObject(root, "ts_ms", (double)s->ts_ms) ||
        !cJSON_AddNumberToObject(root, "uptime_s", (double)s->uptime_s)) {
        goto cleanup;
    }

    if (!cJSON_AddBoolToObject(link, "ble_connected", true) ||
        !cJSON_AddBoolToObject(link, "elm_ready", true) ||
        !cJSON_AddStringToObject(link, "poller", "on") ||
        !cJSON_AddItemToObject(root, "link", link)) {
        goto cleanup;
    }
    link = NULL;

    if (!cJSON_AddNumberToObject(metrics, "cmds_ok", cmds_ok) ||
        !cJSON_AddNumberToObject(metrics, "cmds_fail", s_cmds_fail) ||
        !cJSON_AddNumberToObject(metrics, "ble_reconnects", 0) ||
        !cJSON_AddNumberToObject(metrics, "blocked_cmds", 0) ||
        !cJSON_AddNumberToObject(metrics, "telemetry_drops", 0) ||
        !cJSON_AddItemToObject(root, "metrics", metrics)) {
        goto cleanup;
    }
    metrics = NULL;

    if (!add_sample(samples, "rpm", "010C", s->rpm, "rpm", s->rpm_raw) ||
        !add_sample(samples, "speed", "010D", s->speed_kmh, "km/h", s->speed_raw) ||
        !add_sample(samples, "coolant_c", "0105", s->coolant_c, "C", s->coolant_raw) ||
        !add_sample(samples, "throttle_pct", "0111", s->throttle_pct, "%",
                    s->throttle_raw) ||
        !add_sample(samples, "voltage", "ATRV", s->voltage_v, "V", s->voltage_raw) ||
        !cJSON_AddItemToObject(root, "samples", samples)) {
        goto cleanup;
    }
    samples = NULL;

    text = cJSON_PrintUnformatted(root);

cleanup:
    cJSON_Delete(root);
    cJSON_Delete(link);
    cJSON_Delete(metrics);
    cJSON_Delete(samples);
    return text;
}

static void fleet_tx_task(void *arg)
{
    (void)arg;
    uint32_t sequence = 0;
    TickType_t next_wake = xTaskGetTickCount();
    for (;;) {
        synthetic_snapshot_t snapshot;
        synthetic_snapshot_generate(sequence, (uint64_t)(esp_timer_get_time() / 1000),
                                    &snapshot);
        char *json = build_snapshot_json(&snapshot, s_cmds_ok + 1);
        if (!json) {
            ++s_cmds_fail;
            ESP_LOGE(TAG, "JSON allocation failed");
        } else {
            size_t len = strlen(json);
            int written = uart_write_bytes(FLEET_UART, json, len);
            int newline_written = uart_write_bytes(FLEET_UART, "\n", 1);
            if (written == (int)len && newline_written == 1) {
                ++s_cmds_ok;
                printf("[fleet-tx] %s\n", json);
            } else {
                ++s_cmds_fail;
                ESP_LOGE(TAG, "short UART write: %d/%u", written, (unsigned)len);
            }
            cJSON_free(json);
        }
        ++sequence;
        xTaskDelayUntil(&next_wake, pdMS_TO_TICKS(1000));
    }
}

static void fleet_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[RX_BUF_SIZE + 1];
    for (;;) {
        int len = uart_read_bytes(FLEET_UART, buf, RX_BUF_SIZE,
                                  pdMS_TO_TICKS(200));
        if (len > 0) {
            buf[len] = '\0';
            printf("[fleet-rx] %.*s\n", len, (char *)buf);
        }
    }
}

void app_main(void)
{
    const uart_config_t config = {
        .baud_rate = FLEET_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* TX buffering prevents a ~750-byte frame at 9600 baud from extending
     * the one-second producer cadence. */
    ESP_ERROR_CHECK(uart_driver_install(FLEET_UART, RX_BUF_SIZE * 2, 2048,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(FLEET_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(FLEET_UART, FLEET_TX_GPIO, FLEET_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART1 ready: TX=GPIO%d RX=GPIO%d 9600 8N1",
             FLEET_TX_GPIO, FLEET_RX_GPIO);
    ESP_ERROR_CHECK(xTaskCreate(fleet_tx_task, "fleet_tx", 6144, NULL, 5, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(fleet_rx_task, "fleet_rx", 3072, NULL, 4, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
}
