#pragma once
/* Minimal types for host_registry without a full telemetry bus (v2). */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLEET_MAX_READINGS_PER_REPORT 8

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

typedef enum {
    TELEMETRY_HOST_REPORT = 4,
} telemetry_msg_type_t;

typedef struct {
    telemetry_msg_type_t type;
    telemetry_host_report_t host_report;
} telemetry_msg_t;

static inline void telemetry_publish(const telemetry_msg_t *msg)
{
    (void)msg; /* v2: uplink polls host_registry_snapshot(); no bus */
}

#ifdef __cplusplus
}
#endif
