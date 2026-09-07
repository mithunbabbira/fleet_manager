#include "config_store.h"

#include <Preferences.h>
#include <ctype.h>
#include <string.h>

static const char *NS = "ul212fetch";
static const uint32_t kDefaultPollMs = 1000;
static const uint32_t kDefaultSilenceMs = 10000;

bool configMacValid(const char *mac) {
  if (!mac || strlen(mac) != 17) return false;
  for (int i = 0; i < 17; i++) {
    if (i % 3 == 2) {
      if (mac[i] != ':') return false;
    } else if (!isxdigit((unsigned char)mac[i])) {
      return false;
    }
  }
  return true;
}

void configLoad(AppConfig *out) {
  AppConfig d{};
  d.pollIntervalMs = kDefaultPollMs;
  d.silenceTimeoutMs = kDefaultSilenceMs;

  Preferences p;
  if (!p.begin(NS, true)) {
    *out = d;
    return;
  }

  String mac = p.getString("mac", "");
  mac.toUpperCase();
  strncpy(d.sensorMac, mac.c_str(), sizeof(d.sensorMac) - 1);
  d.pollIntervalMs = p.getUInt("pollMs", kDefaultPollMs);
  d.silenceTimeoutMs = p.getUInt("silMs", kDefaultSilenceMs);
  p.end();

  if (d.pollIntervalMs < 200) d.pollIntervalMs = kDefaultPollMs;
  if (d.silenceTimeoutMs < 3000) d.silenceTimeoutMs = kDefaultSilenceMs;
  *out = d;
}

bool configSave(const AppConfig *cfg) {
  if (!cfg) return false;
  if (cfg->sensorMac[0] && !configMacValid(cfg->sensorMac)) return false;

  Preferences p;
  if (!p.begin(NS, false)) return false;
  p.putString("mac", cfg->sensorMac);
  p.putUInt("pollMs", cfg->pollIntervalMs);
  p.putUInt("silMs", cfg->silenceTimeoutMs);
  p.end();
  return true;
}
