#include "telemetry_bus.h"
#include "sys_runtime.h"

#include <stdbool.h>
#include <string.h>

typedef struct {
    QueueHandle_t queue;
    uint32_t filter_mask;
    bool used;
} telemetry_subscriber_t;

static telemetry_subscriber_t s_subscribers[TELEMETRY_MAX_SUBSCRIBERS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized = false;

esp_err_t telemetry_bus_init(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(s_subscribers, 0, sizeof(s_subscribers));
    s_initialized = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t telemetry_subscribe(QueueHandle_t *out_queue, uint32_t filter_mask)
{
    if (!out_queue) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    QueueHandle_t queue = xQueueCreate(TELEMETRY_QUEUE_DEPTH, sizeof(telemetry_msg_t));
    if (!queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_ERR_NO_MEM; /* subscriber table full */
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < TELEMETRY_MAX_SUBSCRIBERS; ++i) {
        if (!s_subscribers[i].used) {
            s_subscribers[i].used = true;
            s_subscribers[i].queue = queue;
            s_subscribers[i].filter_mask = filter_mask;
            result = ESP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (result != ESP_OK) {
        vQueueDelete(queue);
        return result;
    }

    *out_queue = queue;
    return ESP_OK;
}

esp_err_t telemetry_publish(const telemetry_msg_t *msg)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    QueueHandle_t targets[TELEMETRY_MAX_SUBSCRIBERS];
    int target_count = 0;
    uint32_t bit = TELEMETRY_MASK((uint32_t)msg->type);

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < TELEMETRY_MAX_SUBSCRIBERS; ++i) {
        if (s_subscribers[i].used && (s_subscribers[i].filter_mask & bit)) {
            targets[target_count++] = s_subscribers[i].queue;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    for (int i = 0; i < target_count; ++i) {
        if (xQueueSend(targets[i], msg, 0) != pdPASS) {
            sys_runtime_metric_inc("telemetry_drops");
        }
    }

    return ESP_OK;
}
