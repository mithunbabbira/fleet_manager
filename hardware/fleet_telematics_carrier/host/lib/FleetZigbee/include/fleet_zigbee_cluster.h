#pragma once

#include <stdint.h>

#define FLEET_ZB_HOST_ENDPOINT      10  /* host sends from this endpoint */
#define FLEET_ZB_COORD_ENDPOINT      1  /* coordinator listens here */
#define FLEET_ZB_CLUSTER_ID       0xFC00  /* private cluster, not a standard ZCL cluster */
#define FLEET_ZB_CMD_TLV             0x01  /* command id: raw TLV bytes follow */
#ifndef FLEET_ZB_CHANNEL
#define FLEET_ZB_CHANNEL              15  /* must match CONFIG_FLEET_ZIGBEE_CHANNEL on carrier */
#endif
#ifndef FLEET_ZB_EPAN_ID
#define FLEET_ZB_EPAN_ID "F1EE700000000001"
#endif
