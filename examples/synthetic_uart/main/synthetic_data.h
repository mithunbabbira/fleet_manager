#pragma once

#include <stdint.h>

typedef struct {
    uint32_t sequence;
    uint64_t ts_ms;
    uint64_t uptime_s;
    double rpm;
    uint8_t speed_kmh;
    int16_t coolant_c;
    double throttle_pct;
    double voltage_v;
    char rpm_raw[11];
    char speed_raw[7];
    char coolant_raw[7];
    char throttle_raw[7];
    char voltage_raw[12];
} synthetic_snapshot_t;

void synthetic_snapshot_generate(uint32_t sequence, uint64_t uptime_ms,
                                 synthetic_snapshot_t *out);
