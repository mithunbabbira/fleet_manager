#pragma once

#include <stdint.h>

/*
 * Shared Zigbee "envelope" for fleet TLV frames.
 *
 * Host (end device)  --custom cluster 0xFC00-->  Coordinator (master)
 * Payload inside the cluster command is a fleet_tlv frame (see fleet_tlv.h).
 *
 * Both sides must use the same constants below.
 */
#define FLEET_ZB_HOST_ENDPOINT      10  /* host sends from this endpoint */
#define FLEET_ZB_COORD_ENDPOINT      1  /* coordinator listens here */
#define FLEET_ZB_CLUSTER_ID       0xFC00  /* private cluster, not a standard ZCL cluster */
#define FLEET_ZB_CMD_TLV             0x01  /* command id: raw TLV bytes follow */
#define FLEET_ZB_CHANNEL              15  /* must match CONFIG_FLEET_ZIGBEE_CHANNEL on carrier */
