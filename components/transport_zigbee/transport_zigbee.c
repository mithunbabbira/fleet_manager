#include "transport_zigbee.h"
#include "transport_zigbee_radio.h"

#include "host_registry.h"
#include "sdkconfig.h"
#include "sys_runtime.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "zb_transport";
static bool s_running;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

esp_err_t transport_zigbee_init(void)
{
    host_registry_init();
#if CONFIG_FLEET_ZIGBEE_ENABLE
    esp_err_t err = transport_zigbee_radio_platform_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radio init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "coordinator ready (open network, ch=%d)", CONFIG_FLEET_ZIGBEE_CHANNEL);
#else
    ESP_LOGI(TAG, "loopback mode — use transport_zigbee_ingest() for tests");
#endif
    return ESP_OK;
}

esp_err_t transport_zigbee_start(void)
{
    s_running = true;
#if CONFIG_FLEET_ZIGBEE_ENABLE
    return transport_zigbee_radio_start();
#else
    return ESP_OK;
#endif
}

esp_err_t transport_zigbee_ingest(const uint8_t *frame, size_t len, uint16_t short_addr)
{
    if (!s_running || !frame || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int rc = host_registry_ingest_frame(frame, len, now_ms(), short_addr);
    if (rc == 0) {
        sys_runtime_metric_inc("zigbee_reports");
        return ESP_OK;
    }
    if (rc == -3) {
        sys_runtime_metric_inc("zigbee_unknown_host_type");
    } else if (rc == -1) {
        sys_runtime_metric_inc("zigbee_crc_errors");
    }
    ESP_LOGW(TAG, "ingest failed rc=%d", rc);
    return ESP_FAIL;
}

bool transport_zigbee_is_running(void)
{
    return s_running;
}
