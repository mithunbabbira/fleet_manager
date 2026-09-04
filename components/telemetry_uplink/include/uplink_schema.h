#pragma once

/*
 * Schema ID helpers for Trafyn nc-events-api.
 *
 * Carrier-owned IDs: uplink_schema_ids.h (OBD / GPS).
 * Host-owned IDs: Zigbee TLV SCHEMA_ID on each REPORT/HELLO.
 */

#include "uplink_schema_ids.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Legacy host_type → schema lookup (always NULL).
 * Host events use uplink_host_report_t.schema_id from the wire.
 */
const char *uplink_schema_for_host(uint16_t host_type_id);

#ifdef __cplusplus
}
#endif
