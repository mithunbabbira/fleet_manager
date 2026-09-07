#pragma once
/*
 * Host-testable Zigbee host (schema 1088) payload helpers for firmware_v2 uplink.
 * Flat METRIC_MAP keys: host_type + "key":value + optional key_unit.
 */

#include "uplink_envelope.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPLINK_SCHEMA_HOST "1088"
#define UPLINK_HOST_MAX_READINGS 8

typedef struct {
    char key[24];
    double value;
    char unit[8];
    bool valid;
} uplink_host_reading_t;

typedef struct {
    char device_id[32];
    char node_id[40];
    char schema_id[16];
    char host_type[32];
    uint8_t reading_count;
    uplink_host_reading_t readings[UPLINK_HOST_MAX_READINGS];
} uplink_host_report_t;

/** @brief True if any reading has valid==true and non-empty key. */
bool uplink_host_has_valid_reading(const uplink_host_report_t *host);

/**
 * @brief One host → one payload with all valid readings as flat keys.
 * Returns bytes written or -1.
 */
int uplink_build_host_report_payload(const uplink_host_report_t *host, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
