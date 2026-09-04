#pragma once

/*
 * Trafyn schema IDs for carrier-owned events — edit before building / OTA.
 *
 * Zigbee hosts send their own schemaId on the wire (FLEET_TLV_SCHEMA_ID).
 * Do not add host_type → schema rows here; that mapping lives on each host.
 *
 * NVS `uplink schema` overrides UPLINK_SCHEMA_OBD only (Carrier Console).
 */

/** Vehicle OBD / carrier telemetry. */
#define UPLINK_SCHEMA_OBD "1087"

/** Documented default for UL212 hosts (host firmware / tests — not carrier map). */
#define UPLINK_SCHEMA_HOST_UL212 "1088"

/** GNSS position (virtual gps-* device). */
#define UPLINK_SCHEMA_GPS "1089"

/** Default OBD schema when NVS uplink_schema is empty. */
#define UPLINK_SCHEMA_ID UPLINK_SCHEMA_OBD
