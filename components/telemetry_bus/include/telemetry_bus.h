#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TELEMETRY_PID_SAMPLE = 0,
    TELEMETRY_DTC_LIST,
    TELEMETRY_ELM_EVENT,
    TELEMETRY_ERROR,
    TELEMETRY_HOST_REPORT,
    TELEMETRY_TYPE_COUNT,
} telemetry_msg_type_t;

#define TELEMETRY_MASK(type) (1u << (type))
#define TELEMETRY_MASK_PID_SAMPLE TELEMETRY_MASK(TELEMETRY_PID_SAMPLE)
#define TELEMETRY_MASK_DTC_LIST TELEMETRY_MASK(TELEMETRY_DTC_LIST)
#define TELEMETRY_MASK_ELM_EVENT TELEMETRY_MASK(TELEMETRY_ELM_EVENT)
#define TELEMETRY_MASK_ERROR TELEMETRY_MASK(TELEMETRY_ERROR)
#define TELEMETRY_MASK_HOST_REPORT TELEMETRY_MASK(TELEMETRY_HOST_REPORT)
#define TELEMETRY_MASK_ALL ((uint32_t)((1u << TELEMETRY_TYPE_COUNT) - 1))

#define FLEET_MAX_READINGS_PER_REPORT 8

typedef enum {
    TELEMETRY_ELM_EVENT_CONNECTED = 0,
    TELEMETRY_ELM_EVENT_DISCONNECTED,
    TELEMETRY_ELM_EVENT_INIT_OK,
    TELEMETRY_ELM_EVENT_INIT_FAIL,
} telemetry_elm_event_kind_t;

typedef enum {
    TELEMETRY_ERROR_UNKNOWN = 0,
    TELEMETRY_ERROR_BLE_CONNECT_FAIL,
    TELEMETRY_ERROR_BLE_DISCONNECTED,
    TELEMETRY_ERROR_ELM_TIMEOUT,
    TELEMETRY_ERROR_ELM_NO_DATA,
    TELEMETRY_ERROR_ELM_BUS_ERROR,
    TELEMETRY_ERROR_CMD_BLOCKED,
    TELEMETRY_ERROR_PROFILE_INVALID,
} telemetry_error_code_t;

typedef struct {
    char cmd[16];
    char name[24];
    char raw_hex[48];
    double value;
    char unit[8];
    uint64_t ts_ms;
    bool ok;
} telemetry_pid_sample_t;

typedef struct {
    char codes[8][6];
    int count;
    uint8_t source_mode;
    uint64_t ts_ms;
} telemetry_dtc_list_t;

typedef struct {
    telemetry_elm_event_kind_t kind;
    uint64_t ts_ms;
} telemetry_elm_event_t;

typedef struct {
    telemetry_error_code_t code;
    char cmd[16];
    char message[64];
    uint64_t ts_ms;
} telemetry_error_msg_t;

/* Sized to the manifest limits (FLEET_MANIFEST_KEY_MAX / _TYPE_NAME_MAX) so a
 * long reading key can never be silently truncated on its way to the cloud. */
typedef struct {
    char key[24];
    double value;
    char unit[8];
    bool valid;
} telemetry_host_reading_t;

typedef struct {
    char device_id[32];
    char node_id[40];
    char schema_id[16];
    char host_type[32];
    uint16_t host_type_id;
    uint8_t reading_count;
    telemetry_host_reading_t readings[FLEET_MAX_READINGS_PER_REPORT];
    uint64_t ts_ms;
} telemetry_host_report_t;

typedef struct {
    telemetry_msg_type_t type;
    union {
        telemetry_pid_sample_t pid_sample;
        telemetry_dtc_list_t dtc_list;
        telemetry_elm_event_t elm_event;
        telemetry_error_msg_t error;
        telemetry_host_report_t host_report;
    };
} telemetry_msg_t;

#define TELEMETRY_MAX_SUBSCRIBERS 4
#define TELEMETRY_QUEUE_DEPTH 16

/**
 * @brief Reset in-process PID/event pub-sub subscriber table.
 * @note Call once before subscribe/publish; clears slots under critical section.
 */
esp_err_t telemetry_bus_init(void);

/**
 * @brief Register a filtered subscriber queue (depth TELEMETRY_QUEUE_DEPTH).
 * @note Caller owns the queue; table update is critical-section protected.
 */
esp_err_t telemetry_subscribe(QueueHandle_t *out_queue, uint32_t filter_mask);

/**
 * @brief Non-blocking fan-out of @p msg to matching subscribers.
 * @note Copies under spinlock then xQueueSend(..., 0); drops increment sys_runtime "telemetry_drops".
 */
esp_err_t telemetry_publish(const telemetry_msg_t *msg);

#ifdef __cplusplus
}
#endif
