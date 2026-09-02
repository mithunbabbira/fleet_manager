#pragma once

#include <stdint.h>

/* Runtime configuration persisted in NVS. No sensor MAC is compiled in. */

struct AppConfig {
  char sensorMac[18];       /* "AA:BB:CC:DD:EE:FF" or empty */
  uint32_t pollIntervalMs;  /* default 1000 */
  uint32_t silenceTimeoutMs; /* default 10000 */
};

void configLoad(AppConfig *out);
bool configSave(const AppConfig *cfg);
bool configMacValid(const char *mac);
