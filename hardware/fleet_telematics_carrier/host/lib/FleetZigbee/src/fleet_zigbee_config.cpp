#include "fleet_zigbee_config.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

extern "C" {

static const char *NS = "ul212fetch";
static const uint16_t kDefaultHostTypeId = 1;
static const char *kDefaultMetricMap =
    "16:height_mm:mm:f;17:smooth_mm:mm:f;18:temperature_c:C:f;"
    "19:signal::u8;20:valid_echo::u8;21:tilt_deg::u8";

void fleetZigbeeConfigLoad(FleetZigbeeConfig *out) {
  FleetZigbeeConfig d{};
  strncpy(d.deviceId, "ul212-001", sizeof(d.deviceId) - 1);
  strncpy(d.nodeId, "node-ul212-001", sizeof(d.nodeId) - 1);
  strncpy(d.schemaId, "1088", sizeof(d.schemaId) - 1);
  strncpy(d.hostType, "ul212_ble_fetch", sizeof(d.hostType) - 1);
  strncpy(d.metricMap, kDefaultMetricMap, sizeof(d.metricMap) - 1);
  d.hostTypeId = kDefaultHostTypeId;
  d.manifestVersion = 1;

  Preferences p;
  if (!p.begin(NS, true)) {
    *out = d;
    return;
  }

  String id = p.getString("deviceId", d.deviceId);
  strncpy(d.deviceId, id.c_str(), sizeof(d.deviceId) - 1);
  String node = p.getString("nodeId", d.nodeId);
  strncpy(d.nodeId, node.c_str(), sizeof(d.nodeId) - 1);
  String schema = p.getString("schemaId", d.schemaId);
  strncpy(d.schemaId, schema.c_str(), sizeof(d.schemaId) - 1);
  String hostType = p.getString("hostType", d.hostType);
  strncpy(d.hostType, hostType.c_str(), sizeof(d.hostType) - 1);
  String mmap = p.getString("metricMap", d.metricMap);
  strncpy(d.metricMap, mmap.c_str(), sizeof(d.metricMap) - 1);
  d.hostTypeId = (uint16_t)p.getUInt("hostTypeId", kDefaultHostTypeId);
  d.manifestVersion = (uint8_t)p.getUInt("manifestVer", 1);
  p.end();

  if (!d.nodeId[0]) {
    snprintf(d.nodeId, sizeof(d.nodeId), "node-%s", d.deviceId);
  }
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
  p.putString("nodeId", cfg->nodeId);
  p.putString("schemaId", cfg->schemaId);
  p.putString("hostType", cfg->hostType);
  p.putString("metricMap", cfg->metricMap);
  p.putUInt("hostTypeId", cfg->hostTypeId);
  p.putUInt("manifestVer", cfg->manifestVersion);
  p.end();
  return true;
}

} /* extern "C" */
