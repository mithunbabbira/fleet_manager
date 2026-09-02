#pragma once

/*
 * Trafyn schema IDs — edit this file before building / OTA.
 *
 * Each uplink event type maps to one schemaId on the cloud API.
 * When adding a new sensor host type:
 *   1) Add a #define here (e.g. UPLINK_SCHEMA_HOST_FOO "1090")
 *   2) Add a row in uplink_schema.c (host_type_id → schema id)
 *
 * NVS `uplink schema` overrides UPLINK_SCHEMA_OBD only (Carrier Console).
 * Host and GPS schemas always come from this file.
 */

/** Vehicle OBD / carrier telemetry. */
#define UPLINK_SCHEMA_OBD "1087"

/** UL212 BLE fuel sensor (host_type_id 1). */
#define UPLINK_SCHEMA_HOST_UL212 "1088"

/** GNSS position (virtual gps-* device). */
#define UPLINK_SCHEMA_GPS "1089"

/** Default OBD schema when NVS uplink_schema is empty. */
#define UPLINK_SCHEMA_ID UPLINK_SCHEMA_OBD
