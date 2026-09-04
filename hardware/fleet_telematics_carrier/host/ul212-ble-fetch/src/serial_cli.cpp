#include "serial_cli.h"

#include "ble_ul212.h"
#include "config_store.h"
#include "fleet_zigbee_config.h"
#include "fleet_zigbee_ed.h"

#include <Arduino.h>
#include <ctype.h>
#include <string.h>

static AppConfig s_app;
static FleetZigbeeConfig s_zb;

static void trim(char *s) {
  if (!s) return;
  while (*s && isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static void toUpperMac(char *mac) {
  for (char *p = mac; *p; p++) *p = toupper((unsigned char)*p);
}

static void printHelp() {
  Serial.println(
      "Commands:\n"
      "  help              this list\n"
      "  status            BLE reading + Zigbee join state\n"
      "  config            NVS settings (mac, id, poll)\n"
      "  scan              BLE scan ~5s for UL212 (service 0xFFE0)\n"
      "  mac AA:BB:...     set sensor MAC (save to persist)\n"
      "  id ul212-001      set Zigbee device_id (also refreshes default node_id)\n"
      "  node node-ul212-001  set cloud node_id\n"
      "  schema 1088       set cloud schemaId\n"
      "  poll <ms>         poll interval, min 200 (save to persist)\n"
      "  silence <ms>      silence timeout, min 3000 (save to persist)\n"
      "  save              write NVS and reboot\n"
      "  reboot            restart firmware");
}

static void printConfig() {
  Serial.printf("mac=%s\n", s_app.sensorMac[0] ? s_app.sensorMac : "(not set)");
  Serial.printf("poll=%lu ms silence=%lu ms\n", (unsigned long)s_app.pollIntervalMs,
                (unsigned long)s_app.silenceTimeoutMs);
  Serial.printf("device_id=%s node_id=%s schema_id=%s host_type=%s host_type_id=%u\n",
                s_zb.deviceId, s_zb.nodeId, s_zb.schemaId, s_zb.hostType,
                (unsigned)s_zb.hostTypeId);
}

static void printStatus() {
  printConfig();
  Serial.printf("ble_connected=%s configured=%s\n",
                bleUl212Connected() ? "yes" : "no",
                bleUl212Configured() ? "yes" : "no");
#if defined(FLEET_ZIGBEE_ED_RADIO) && FLEET_ZIGBEE_ED_RADIO
  Serial.printf("zigbee_joined=%s\n", fleetZigbeeEdIsJoined() ? "yes" : "no");
#endif
  const Ul212Reading r = bleUl212Latest();
  if (r.valid) {
    Serial.printf("height_mm=%.1f smooth_mm=%.1f signal=%u temp=%.1f C tilt=%u deg\n",
                  r.heightMm, r.smoothMm, r.signal, r.temperatureC, r.tiltDeg);
  } else {
    Serial.println("reading=(none yet)");
  }
}

static void cmdScan() {
  Serial.println("scanning 5s for UL212 (0xFFE0)...");
  BleScanEntry found[8];
  const size_t n = bleUl212Scan(found, 8);
  if (n == 0) {
    Serial.println("scan: no devices found");
    return;
  }
  for (size_t i = 0; i < n; i++) {
    Serial.printf("  %s  %d dBm  %s\n", found[i].mac, found[i].rssi, found[i].name);
  }
}

static bool cmdSave() {
  if (s_app.sensorMac[0] && !configMacValid(s_app.sensorMac)) {
    Serial.println("save: invalid mac");
    return false;
  }
  if (!s_zb.deviceId[0]) {
    Serial.println("save: device_id empty");
    return false;
  }
  if (!configSave(&s_app)) {
    Serial.println("save: app config failed");
    return false;
  }
  if (!fleetZigbeeConfigSave(&s_zb)) {
    Serial.println("save: zigbee config failed");
    return false;
  }
  Serial.println("save: ok — rebooting");
  delay(300);
  ESP.restart();
  return true;
}

static void dispatch(char *line) {
  trim(line);
  if (!line[0]) return;

  char *sp = strchr(line, ' ');
  if (sp) {
    *sp++ = '\0';
    trim(sp);
  } else {
    sp = (char *)"";
  }

  if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
    printHelp();
  } else if (strcmp(line, "status") == 0) {
    printStatus();
  } else if (strcmp(line, "config") == 0) {
    printConfig();
  } else if (strcmp(line, "scan") == 0) {
    cmdScan();
  } else if (strcmp(line, "mac") == 0) {
    if (!sp[0]) {
      Serial.println("usage: mac AA:BB:CC:DD:EE:FF");
      return;
    }
    toUpperMac(sp);
    if (!configMacValid(sp)) {
      Serial.println("invalid mac");
      return;
    }
    strncpy(s_app.sensorMac, sp, sizeof(s_app.sensorMac) - 1);
    Serial.printf("mac set to %s (run save to persist)\n", s_app.sensorMac);
  } else if (strcmp(line, "id") == 0) {
    if (!sp[0] || strlen(sp) >= FLEET_ZB_DEVICE_ID_MAX) {
      Serial.println("usage: id ul212-001");
      return;
    }
    strncpy(s_zb.deviceId, sp, sizeof(s_zb.deviceId) - 1);
    snprintf(s_zb.nodeId, sizeof(s_zb.nodeId), "node-%s", s_zb.deviceId);
    Serial.printf("device_id=%s node_id=%s (run save to persist)\n", s_zb.deviceId, s_zb.nodeId);
  } else if (strcmp(line, "node") == 0) {
    if (!sp[0] || strlen(sp) >= FLEET_ZB_NODE_ID_MAX) {
      Serial.println("usage: node node-ul212-001");
      return;
    }
    strncpy(s_zb.nodeId, sp, sizeof(s_zb.nodeId) - 1);
    Serial.printf("node_id set to %s (run save to persist)\n", s_zb.nodeId);
  } else if (strcmp(line, "schema") == 0) {
    if (!sp[0] || strlen(sp) >= FLEET_ZB_SCHEMA_ID_MAX) {
      Serial.println("usage: schema 1088");
      return;
    }
    strncpy(s_zb.schemaId, sp, sizeof(s_zb.schemaId) - 1);
    Serial.printf("schema_id set to %s (run save to persist)\n", s_zb.schemaId);
  } else if (strcmp(line, "poll") == 0) {
    const uint32_t v = (uint32_t)strtoul(sp, nullptr, 10);
    if (v < 200) {
      Serial.println("usage: poll <ms> (min 200)");
      return;
    }
    s_app.pollIntervalMs = v;
    Serial.printf("poll=%lu ms (run save to persist)\n", (unsigned long)v);
  } else if (strcmp(line, "silence") == 0) {
    const uint32_t v = (uint32_t)strtoul(sp, nullptr, 10);
    if (v < 3000) {
      Serial.println("usage: silence <ms> (min 3000)");
      return;
    }
    s_app.silenceTimeoutMs = v;
    Serial.printf("silence=%lu ms (run save to persist)\n", (unsigned long)v);
  } else if (strcmp(line, "save") == 0) {
    (void)cmdSave();
  } else if (strcmp(line, "reboot") == 0) {
    Serial.println("rebooting...");
    delay(200);
    ESP.restart();
  } else {
    Serial.printf("unknown: %s (try help)\n", line);
  }
}

void serialCliBegin() {
  configLoad(&s_app);
  fleetZigbeeConfigLoad(&s_zb);
}

void serialCliTask(void *param) {
  (void)param;
  static char line[128];

  for (;;) {
    if (Serial.available()) {
      const int n = Serial.readBytesUntil('\n', line, sizeof(line) - 1);
      line[n] = '\0';
      if (n > 0 && line[n - 1] == '\r') line[n - 1] = '\0';
      dispatch(line);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
