#include "zigbee_report.h"

#include "ble_ul212.h"
#include "fleet_zigbee_ed.h"

#include <Arduino.h>

static uint8_t s_seq;

void zigbeeReportBegin(void)
{
    FleetZigbeeConfig cfg;
    fleetZigbeeConfigLoad(&cfg);
    fleetZigbeeEdBegin(&cfg);
}

void zigbeeReportTask(void *param)
{
    (void)param;
    zigbeeReportBegin();

    for (;;) {
        fleetZigbeeEdLoop();

        if (fleetZigbeeEdIsJoined()) {
            const Ul212Reading r = bleUl212Latest();
            FleetZigbeeReading readings[6];
            size_t n = 0;

            readings[n++] = {16, FLEET_VAL_FLOAT, {.f32 = r.heightMm}};
            readings[n++] = {17, FLEET_VAL_FLOAT, {.f32 = r.smoothMm}};
            readings[n++] = {18, FLEET_VAL_FLOAT, {.f32 = r.temperatureC}};
            readings[n++] = {19, FLEET_VAL_UINT8, {.u8 = r.signal}};
            readings[n++] = {20, FLEET_VAL_UINT8,
                             {.u8 = (r.signal > 0 && r.validSignals > 0) ? 1u : 0u}};
            readings[n++] = {21, FLEET_VAL_UINT8, {.u8 = r.tiltDeg}};

            uint8_t status = 0;
            if (bleUl212Connected()) {
                status |= FLEET_STATUS_SENSOR_CONNECTED;
            }
            if (r.valid) {
                status |= FLEET_STATUS_READING_VALID;
            }
            fleetZigbeeEdSendReport(readings, n, status, ++s_seq);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
