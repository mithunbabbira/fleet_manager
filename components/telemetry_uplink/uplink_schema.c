#include "uplink_schema.h"

#include <stddef.h>

typedef struct {
    uint16_t host_type_id;
    const char *schema_id;
} uplink_host_schema_entry_t;

static const uplink_host_schema_entry_t s_host_schemas[] = {
    {1, UPLINK_SCHEMA_HOST_UL212},
};

const char *uplink_schema_for_host(uint16_t host_type_id)
{
    for (size_t i = 0; i < sizeof(s_host_schemas) / sizeof(s_host_schemas[0]); i++) {
        if (s_host_schemas[i].host_type_id == host_type_id) {
            return s_host_schemas[i].schema_id;
        }
    }
    return NULL;
}
