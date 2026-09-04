#include "uplink_schema.h"

#include <stddef.h>

/*
 * Host schema IDs are owned by Zigbee hosts (TLV SCHEMA_ID). Carrier only
 * hardcodes OBD/GPS in uplink_schema_ids.h.
 */
const char *uplink_schema_for_host(uint16_t host_type_id)
{
    (void)host_type_id;
    return NULL;
}
