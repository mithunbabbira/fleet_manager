#pragma once
/*
 * Zigbee identity / RF — edit this file per board / truck, then rebuild.
 * Truck isolation = same channel + same EPAN as that truck's carrier.
 * Nearby trucks: use different channel and EPAN so hosts cannot join the wrong parent.
 * device_id must be unique per host for registry/cloud segregation.
 */

#ifndef FLEET_ZB_CHANNEL
#define FLEET_ZB_CHANNEL 15
#endif

/* 16 hex digits, MSB-first — must match CONFIG_FLEET_ZIGBEE_EPAN_ID on the carrier. */
#ifndef FLEET_ZB_EPAN_ID
#define FLEET_ZB_EPAN_ID "F1EE700000000001"
#endif

#ifndef ZB_DEVICE_ID
#define ZB_DEVICE_ID "ul212-rs232-001"
#endif

#ifndef ZB_NODE_ID
#define ZB_NODE_ID "node-ul212-rs232-001"
#endif

#ifndef ZB_SCHEMA_ID
#define ZB_SCHEMA_ID "1088"
#endif

#ifndef ZB_HOST_TYPE
#define ZB_HOST_TYPE "ul212_rs232_fetch"
#endif

#ifndef ZB_METRIC_MAP
#define ZB_METRIC_MAP                                                  \
  "16:height_mm:mm:f;17:smooth_mm:mm:f;18:temperature_c:C:f;"          \
  "19:signal::u8;20:valid_echo::u8;21:tilt_deg::u8"
#endif
