/* host_registry table-full behavior: FLEET_REGISTRY_MAX_HOSTS caps the
 * table, and a new distinct host joining must recycle the
 * least-recently-seen slot rather than being dropped or corrupting the
 * table (see find_or_alloc_host() in host_registry.c). */
#include "fleet_tlv.h"
#include "host_registry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void ingest_hello(const char *device_id, uint64_t now_ms)
{
    fleet_encode_input_t in;
    memset(&in, 0, sizeof(in));
    in.msg_type = FLEET_MSG_HELLO;
    strncpy(in.header.device_id, device_id, sizeof(in.header.device_id) - 1);
    in.header.host_type_id = 999; /* matches the "template" fixture manifest entry */
    in.header.seq = 1;
    in.header.ts_ms = now_ms;
    in.header.status = FLEET_STATUS_SENSOR_CONNECTED;
    in.header.manifest_version = 1;

    uint8_t buf[FLEET_TLV_MAX_FRAME];
    int n = fleet_tlv_encode(&in, buf, sizeof(buf));
    assert(n > 0);
    assert(host_registry_ingest_frame_ex(buf, (size_t)n, now_ms, 0x1000, false) == 0);
}

static bool snapshot_has(const fleet_registry_snapshot_t *snap, const char *device_id)
{
    for (uint8_t i = 0; i < snap->host_count; i++) {
        if (strcmp(snap->hosts[i].device_id, device_id) == 0) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    host_registry_init();

    char device_id[32];
    for (int i = 0; i < FLEET_REGISTRY_MAX_HOSTS; i++) {
        snprintf(device_id, sizeof(device_id), "dev-%02d", i);
        ingest_hello(device_id, 1000 + (uint64_t)i);
    }

    fleet_registry_snapshot_t snap;
    assert(host_registry_snapshot(&snap));
    assert(snap.host_count == FLEET_REGISTRY_MAX_HOSTS);
    assert(snapshot_has(&snap, "dev-00"));

    /* Table is full; a new distinct device joining must recycle the
     * least-recently-seen slot (dev-00, last_seen_ms 1000) instead of being
     * rejected or growing the table past its bound. */
    ingest_hello("dev-new", 5000);

    assert(host_registry_snapshot(&snap));
    assert(snap.host_count == FLEET_REGISTRY_MAX_HOSTS);
    assert(!snapshot_has(&snap, "dev-00"));
    assert(snapshot_has(&snap, "dev-new"));
    assert(snapshot_has(&snap, "dev-01")); /* everything else survives */
    assert(snapshot_has(&snap, "dev-15"));

    printf("test_host_registry_capacity_v2: OK\n");
    return 0;
}
