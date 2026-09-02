#pragma once

#include "esp_shim.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TELEMETRY_PID_SAMPLE = 0,
    TELEMETRY_DTC_LIST,
    TELEMETRY_ELM_EVENT,
    TELEMETRY_ERROR,
    TELEMETRY_HOST_REPORT,
    TELEMETRY_TYPE_COUNT,
} telemetry_msg_type_t;

#define FLEET_MAX_READINGS_PER_REPORT 8

typedef struct {
    char key[20];
    double value;
    char unit[8];
    bool valid;
} telemetry_host_reading_t;

typedef struct {
    char device_id[32];
    char host_type[24];
    uint16_t host_type_id;
    uint8_t reading_count;
    telemetry_host_reading_t readings[FLEET_MAX_READINGS_PER_REPORT];
    uint64_t ts_ms;
} telemetry_host_report_t;

typedef struct {
    telemetry_msg_type_t type;
    union {
        char _pad;
        telemetry_host_report_t host_report;
    };
} telemetry_msg_t;

esp_err_t telemetry_publish(const telemetry_msg_t *msg);
