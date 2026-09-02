#include "fleet_zigbee_config.h"

#include <Preferences.h>
#include <string.h>

extern "C" {

static const char *NS = "ul212fetch";
static const uint16_t kDefaultHostTypeId = 1;

void fleetZigbeeConfigLoad(FleetZigbeeConfig *out) {
  FleetZigbeeConfig d{};
  strncpy(d.deviceId, "ul212-001", sizeof(d.deviceId) - 1);
  d.hostTypeId = kDefaultHostTypeId;
  d.manifestVersion = 1;

  Preferences p;
  if (!p.begin(NS, true)) {
    *out = d;
    return;
  }

  String id = p.getString("deviceId", d.deviceId);
  strncpy(d.deviceId, id.c_str(), sizeof(d.deviceId) - 1);
  d.hostTypeId = (uint16_t)p.getUInt("hostTypeId", kDefaultHostTypeId);
  d.manifestVersion = (uint8_t)p.getUInt("manifestVer", 1);
  p.end();
  *out = d;
}

bool fleetZigbeeConfigSave(const FleetZigbeeConfig *cfg) {
  if (!cfg || cfg->deviceId[0] == '\0') {
    return false;
  }

  Preferences p;
  if (!p.begin(NS, false)) {
    return false;
  }
  p.putString("deviceId", cfg->deviceId);
  p.putUInt("hostTypeId", cfg->hostTypeId);
  p.putUInt("manifestVer", cfg->manifestVersion);
  p.end();
  return true;
}

} /* extern "C" */
