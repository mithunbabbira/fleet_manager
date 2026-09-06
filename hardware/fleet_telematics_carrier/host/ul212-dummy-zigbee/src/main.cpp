#include <Arduino.h>
#include <math.h>

#include "dummy_reading.h"
#include "zigbee_app_config.h"
#include "zigbee_report.h"

static void synthesize(DummyFuelReading *out)
{
  const uint32_t now = millis();
  const float t = (float)now / 1000.0f;
  /* ~80 mm ± 10 mm over ~60 s */
  const float height = 80.0f + 10.0f * sinf(t * (2.0f * 3.1415926f / 60.0f));
  out->valid = true;
  out->height_mm = height;
  out->smooth_mm = height;
  out->temp_c = 30.0f + 2.0f * sinf(t * (2.0f * 3.1415926f / 120.0f));
  out->signal = 30;
  out->tilt_deg = (uint8_t)(3 + ((now / 5000u) % 3u));
  out->stamp_ms = now;
}

void setup()
{
  Serial.begin(115200);
  delay(1500);
  Serial.println("=== UL212 dummy Zigbee (ESP32-C6 Super Mini) ===");
  Serial.printf("device=%s node=%s ch=%d epan=%s\n", ZB_DEVICE_ID, ZB_NODE_ID, FLEET_ZB_CHANNEL,
                FLEET_ZB_EPAN_ID);

  DummyFuelReading first{};
  synthesize(&first);
  zigbeeReportUpdate(&first);
  zigbeeReportStart();
}

void loop()
{
  DummyFuelReading r{};
  synthesize(&r);
  zigbeeReportUpdate(&r);
  Serial.printf("dummy height=%.1f mm temp=%.1f C tilt=%u\n", r.height_mm, r.temp_c, r.tilt_deg);
  delay(1000);
}
