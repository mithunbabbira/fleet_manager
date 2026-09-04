#include "zigbee_report.h"

#include "fleet_zigbee_ed.h"
#include "zigbee_app_config.h"

#include <Arduino.h>
#include <string.h>

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static Ul212Reading s_latest{};
static bool s_started;

static const uint32_t ZB_REPORT_PERIOD_MS = 5000;
static const uint32_t ZB_LOOP_MS = 200;

static Ul212Reading copyLatest(void)
{
  Ul212Reading r;
  portENTER_CRITICAL(&s_mux);
  r = s_latest;
  portEXIT_CRITICAL(&s_mux);
  return r;
}

void zigbeeReportUpdate(const Ul212Reading *r)
{
  if (!r || !r->valid) {
    return;
  }
  portENTER_CRITICAL(&s_mux);
  s_latest = *r;
  portEXIT_CRITICAL(&s_mux);
}

static void fillConfig(FleetZigbeeConfig *cfg)
{
  memset(cfg, 0, sizeof(*cfg));
  strncpy(cfg->deviceId, ZB_DEVICE_ID, sizeof(cfg->deviceId) - 1);
  strncpy(cfg->nodeId, ZB_NODE_ID, sizeof(cfg->nodeId) - 1);
  strncpy(cfg->schemaId, ZB_SCHEMA_ID, sizeof(cfg->schemaId) - 1);
  strncpy(cfg->hostType, ZB_HOST_TYPE, sizeof(cfg->hostType) - 1);
  strncpy(cfg->metricMap, ZB_METRIC_MAP, sizeof(cfg->metricMap) - 1);
  cfg->hostTypeId = 1;
  cfg->manifestVersion = 1;
}

static void zigbeeTask(void *param)
{
  (void)param;
  uint32_t last_wait_log_ms = 0;

  for (;;) {
    if (copyLatest().valid) {
      break;
    }
    const uint32_t now = millis();
    if (last_wait_log_ms == 0 || (now - last_wait_log_ms) >= 10000) {
      last_wait_log_ms = now;
      Serial.println("[zb] waiting for first valid RS-232 reading…");
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  FleetZigbeeConfig cfg;
  fillConfig(&cfg);
  Serial.printf("[zb] start device=%s ch=%d\n", cfg.deviceId, FLEET_ZB_CHANNEL);
  fleetZigbeeEdBegin(&cfg);

  uint8_t seq = 0;
  uint32_t last_report_ms = 0;

  for (;;) {
    fleetZigbeeEdLoop();

    const Ul212Reading r = copyLatest();
    const uint32_t now = millis();
    if (fleetZigbeeEdIsJoined() && r.valid &&
        (last_report_ms == 0 || (now - last_report_ms) >= ZB_REPORT_PERIOD_MS)) {
      FleetZigbeeReading readings[6];
      size_t n = 0;
      readings[n++] = {16, FLEET_VAL_FLOAT, {.f32 = r.height_mm}};
      readings[n++] = {17, FLEET_VAL_FLOAT, {.f32 = r.smooth_mm}};
      readings[n++] = {18, FLEET_VAL_FLOAT, {.f32 = r.temp_c}};
      readings[n++] = {19, FLEET_VAL_UINT8, {.u8 = r.signal}};
      readings[n++] = {20, FLEET_VAL_UINT8, {.u8 = (uint8_t)((r.signal > 0) ? 1u : 0u)}};
      readings[n++] = {21, FLEET_VAL_UINT8, {.u8 = r.tilt_deg}};

      uint8_t status = FLEET_STATUS_SENSOR_CONNECTED;
      if (r.valid) {
        status |= FLEET_STATUS_READING_VALID;
      }

      const bool ok = fleetZigbeeEdSendReport(readings, n, status, ++seq);
      last_report_ms = millis();
      if (ok) {
        Serial.printf("[zb] report seq=%u height=%.1f\n", (unsigned)seq, r.height_mm);
      } else {
        Serial.println("[zb] report TX failed (will retry)");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(ZB_LOOP_MS));
  }
}

void zigbeeReportStart(void)
{
  if (s_started) {
    return;
  }
  if (xTaskCreate(zigbeeTask, "zb_rpt", 8192, nullptr, 5, nullptr) != pdPASS) {
    Serial.println("[zb] failed to create report task");
    return;
  }
  s_started = true;
}
