#include "zigbee_report.h"

#include "ble_ul212.h"
#include "fleet_zigbee_ed.h"

#include <Arduino.h>

static uint8_t s_seq;

/* Report less often than BLE poll; carve a short RF window for Zigbee TX. */
static const uint32_t ZB_REPORT_PERIOD_MS = 5000;
static const uint32_t ZB_AIR_SLICE_MS = 280;
static const uint32_t ZB_LOOP_MS = 200;

void zigbeeReportBegin(void)
{
    FleetZigbeeConfig cfg;
    fleetZigbeeConfigLoad(&cfg);
    fleetZigbeeEdBegin(&cfg);
}

void zigbeeReportTask(void *param)
{
    (void)param;

    /* BLE-first: do not start Zigbee until the fuel sensor is linked and we
     * have a valid reading. HELLO without payload wastes RF and fights BLE. */
    uint32_t last_wait_log_ms = 0;
    for (;;) {
        const bool configured = bleUl212Configured();
        const bool connected = bleUl212Connected();
        const bool have_reading = bleUl212Latest().valid;
        if (configured && connected && have_reading) {
            break;
        }
        const uint32_t now = millis();
        if (last_wait_log_ms == 0 || (now - last_wait_log_ms) >= 10000) {
            last_wait_log_ms = now;
            if (!configured) {
                Serial.println("[zb] waiting for sensor MAC (scan / mac / save)");
            } else if (!connected) {
                Serial.println("[zb] waiting for BLE sensor connection…");
            } else {
                Serial.println("[zb] BLE up — waiting for first valid reading…");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    Serial.println("[zb] BLE sensor ready — starting Zigbee stack");
    zigbeeReportBegin();

    uint32_t last_report_ms = 0;

    for (;;) {
        fleetZigbeeEdLoop();

        const uint32_t now = millis();
        if (fleetZigbeeEdIsJoined() && bleUl212Latest().valid &&
            (last_report_ms == 0 || (now - last_report_ms) >= ZB_REPORT_PERIOD_MS)) {
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

            /* Pause BLE Modbus polls and bias RF toward Zigbee for one TX window. */
            bleUl212PausePolling(true);
            bleUl212PreferZigbeeAirtime(true);
            vTaskDelay(pdMS_TO_TICKS(40));

            const bool ok = fleetZigbeeEdSendReport(readings, n, status, ++s_seq);
            vTaskDelay(pdMS_TO_TICKS(ZB_AIR_SLICE_MS));

            bleUl212PreferZigbeeAirtime(false);
            bleUl212PausePolling(false);

            last_report_ms = millis();
            if (ok) {
                Serial.printf("[zb] report seq=%u height=%.1f\n", (unsigned)s_seq, r.heightMm);
            } else {
                Serial.println("[zb] report TX failed (join kept; will retry)");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ZB_LOOP_MS));
    }
}
