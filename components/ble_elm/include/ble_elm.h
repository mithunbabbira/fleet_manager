#pragma once
#include "elm_transport.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[32];
    uint8_t addr[6];
    uint8_t addr_type; /* BLE_ADDR_PUBLIC or BLE_ADDR_RANDOM */
    int8_t rssi;
} ble_elm_device_t;

esp_err_t ble_elm_init(void);
esp_err_t ble_elm_start_scan(uint32_t duration_ms);
esp_err_t ble_elm_get_scan_results(ble_elm_device_t *out, int max, int *count);
esp_err_t ble_elm_connect_addr(const uint8_t addr[6]);
esp_err_t ble_elm_disconnect(void);
bool ble_elm_is_connected(void);
/** Returns transport pointer; is_ready() false until GATT NUS discovered. */
elm_transport_t *ble_elm_get_transport(void);
/** Optional: start background reconnect to bonded addr with exp backoff 1..30s */
esp_err_t ble_elm_start_auto_reconnect(void);
esp_err_t ble_elm_stop_auto_reconnect(void);

#ifdef __cplusplus
}
#endif
