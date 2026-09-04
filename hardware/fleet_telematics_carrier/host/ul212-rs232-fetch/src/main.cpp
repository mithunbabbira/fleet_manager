#include <Arduino.h>
#include "board_pins.h"
#include "ul212_rs232.h"
#include "ul212_rs232_config.h"
#include "zigbee_app_config.h"
#include "zigbee_report.h"

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("=== UL212 RS-232 fetch (XIAO ESP32-C6) ===");
  Serial.printf("protocol=%d address=%d RX=%d TX=%d\n",
                UL212_RS232_PROTOCOL, UL212_RS232_ADDRESS,
                UL212_UART_RX_PIN, UL212_UART_TX_PIN);
  Serial.printf("zigbee device=%s ch=%d (no Bluetooth)\n",
                ZB_DEVICE_ID, FLEET_ZB_CHANNEL);
  ul212Rs232Begin(UL212_UART_RX_PIN, UL212_UART_TX_PIN, 9600);
  zigbeeReportStart();
}

void loop() {
  Ul212Reading r = ul212Rs232Poll(1000);
  if (r.valid) {
    zigbeeReportUpdate(&r);
    Serial.printf("height=%.1f mm  smooth=%.1f mm  temp=%.1f C  tilt=%u deg  signal=%u\n",
                  r.height_mm, r.smooth_mm, r.temp_c, r.tilt_deg, r.signal);
  } else {
    Serial.println("(no frame)");
  }
#if UL212_RS232_PROTOCOL == 51
  delay(200);
#else
  delay(500);
#endif
}
