#include "synthetic_data.h"

#include <stdio.h>
#include <string.h>

void synthetic_snapshot_generate(uint32_t sequence, uint64_t uptime_ms,
                                 synthetic_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->sequence = sequence;
    out->ts_ms = uptime_ms;
    out->uptime_s = uptime_ms / 1000U;

    uint32_t phase = sequence % 160U;
    uint32_t ramp = phase <= 80U ? phase : 160U - phase;

    out->speed_kmh = (uint8_t)ramp;
    out->rpm = 780.0 + (double)ramp * 20.0;
    out->coolant_c = (int16_t)(85 + (sequence % 11U));
    out->throttle_pct = 10.0 + (double)(sequence % 51U);
    out->voltage_v = 13.5 + (double)(sequence % 8U) / 10.0;

    uint16_t rpm_x4 = (uint16_t)(out->rpm * 4.0);
    uint8_t coolant_a = (uint8_t)(out->coolant_c + 40);
    uint8_t throttle_a = (uint8_t)((out->throttle_pct * 255.0 / 100.0) + 0.5);

    snprintf(out->rpm_raw, sizeof(out->rpm_raw), "410C%02X%02X",
             (rpm_x4 >> 8) & 0xFF, rpm_x4 & 0xFF);
    snprintf(out->speed_raw, sizeof(out->speed_raw), "410D%02X", out->speed_kmh);
    snprintf(out->coolant_raw, sizeof(out->coolant_raw), "4105%02X", coolant_a);
    snprintf(out->throttle_raw, sizeof(out->throttle_raw), "4111%02X", throttle_a);
    snprintf(out->voltage_raw, sizeof(out->voltage_raw), "%.1fV", out->voltage_v);
}
