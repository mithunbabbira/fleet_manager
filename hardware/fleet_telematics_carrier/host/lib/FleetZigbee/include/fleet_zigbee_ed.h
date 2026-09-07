#pragma once

#include "fleet_tlv.h"
#include "fleet_zigbee_config.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint16_t tlvId;
  fleet_value_type_t type;
  union {
    float f32;
    uint8_t u8;
    uint16_t u16;
    int32_t i32;
  } value;
} FleetZigbeeReading;

void fleetZigbeeEdBegin(const FleetZigbeeConfig *cfg);
bool fleetZigbeeEdSendHello(void);
bool fleetZigbeeEdSendReport(const FleetZigbeeReading *readings, size_t count,
                             uint8_t status, uint8_t seq);
bool fleetZigbeeEdIsJoined(void);
void fleetZigbeeEdLoop(void);

#ifdef __cplusplus
}
#endif
