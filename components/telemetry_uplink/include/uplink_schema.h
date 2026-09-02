#pragma once

/*
 * Schema ID registry for Trafyn nc-events-api.
 *
 * To change schema numbers: edit uplink_schema_ids.h and rebuild.
 * Host type → schema mapping: uplink_schema.c
 */

#include "uplink_schema_ids.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Schema id for a Zigbee host type; NULL if unknown. */
const char *uplink_schema_for_host(uint16_t host_type_id);

#ifdef __cplusplus
}
#endif
