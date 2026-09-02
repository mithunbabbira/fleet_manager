#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLEET_ZB_DEVICE_ID_MAX 32

typedef struct {
  char deviceId[FLEET_ZB_DEVICE_ID_MAX];
  uint16_t hostTypeId;
  uint8_t manifestVersion;
} FleetZigbeeConfig;

void fleetZigbeeConfigLoad(FleetZigbeeConfig *out);
bool fleetZigbeeConfigSave(const FleetZigbeeConfig *cfg);

#ifdef __cplusplus
}
#endif
