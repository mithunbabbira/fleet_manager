#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLEET_ZB_DEVICE_ID_MAX 32
#define FLEET_ZB_NODE_ID_MAX 40
#define FLEET_ZB_SCHEMA_ID_MAX 16
#define FLEET_ZB_HOST_TYPE_MAX 32
#define FLEET_ZB_METRIC_MAP_MAX 160

typedef struct {
  char deviceId[FLEET_ZB_DEVICE_ID_MAX];
  char nodeId[FLEET_ZB_NODE_ID_MAX];
  char schemaId[FLEET_ZB_SCHEMA_ID_MAX];
  char hostType[FLEET_ZB_HOST_TYPE_MAX];
  char metricMap[FLEET_ZB_METRIC_MAP_MAX];
  uint16_t hostTypeId;
  uint8_t manifestVersion;
} FleetZigbeeConfig;

void fleetZigbeeConfigLoad(FleetZigbeeConfig *out);
bool fleetZigbeeConfigSave(const FleetZigbeeConfig *cfg);

#ifdef __cplusplus
}
#endif
