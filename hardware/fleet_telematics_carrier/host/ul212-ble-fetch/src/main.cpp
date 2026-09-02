#include <Arduino.h>

#include "ble_ul212.h"
#include "config_store.h"
#include "serial_cli.h"
#include "zigbee_report.h"

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== UL212 BLE Fetch (serial provisioning) ===");

  serialCliBegin();
  AppConfig cfg;
  configLoad(&cfg);
  Serial.printf("[cfg] mac=%s poll=%lu ms silence=%lu ms\n",
                cfg.sensorMac[0] ? cfg.sensorMac : "(not set — run scan / mac / save)",
                (unsigned long)cfg.pollIntervalMs,
                (unsigned long)cfg.silenceTimeoutMs);
  Serial.println("Type help for USB serial commands");

  bleUl212Begin();
  if (cfg.sensorMac[0]) {
    bleUl212ApplyConfig(cfg.sensorMac, cfg.pollIntervalMs, cfg.silenceTimeoutMs);
  }

  xTaskCreate(serialCliTask, "cli", 8192, nullptr, 3, nullptr);
  xTaskCreate(bleUl212Task, "ble", 8192, nullptr, 5, nullptr);
#if defined(FLEET_ZIGBEE_ED_RADIO) && FLEET_ZIGBEE_ED_RADIO
  xTaskCreate(zigbeeReportTask, "zb_rep", 12288, nullptr, 4, nullptr);
#else
  xTaskCreate(zigbeeReportTask, "zb_rep", 4096, nullptr, 4, nullptr);
#endif
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
