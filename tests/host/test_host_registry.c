#include "fleet_tlv.h"
#include "host_registry.h"

#ifdef HOST_REGISTRY_HOST_TEST
#include "telemetry_bus_shim.h"
#else
#include "telemetry_bus.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

esp_err_t telemetry_bus_init(void)
{
    return 0;
}

esp_err_t telemetry_subscribe(QueueHandle_t *out_queue, uint32_t filter_mask)
{
    (void)out_queue;
    (void)filter_mask;
    return 0;
}

esp_err_t telemetry_publish(const telemetry_msg_t *msg)
{
    (void)msg;
    return 0;
}

int main(void)
{
    host_registry_init();

    fleet_encode_input_t in;
    memset(&in, 0, sizeof(in));
    in.msg_type = FLEET_MSG_HELLO;
    strncpy(in.header.device_id, "template-001", sizeof(in.header.device_id) - 1);
    in.header.host_type_id = 999;
    in.header.seq = 1;
    in.header.ts_ms = 1000;
    in.header.status = FLEET_STATUS_SENSOR_CONNECTED;
    in.header.manifest_version = 1;

    uint8_t buf[FLEET_TLV_MAX_FRAME];
    int n = fleet_tlv_encode(&in, buf, sizeof(buf));
    assert(n > 0);
    assert(host_registry_ingest_frame_ex(buf, (size_t)n, 2000, 0x1234, false) == 0);

    fleet_registry_snapshot_t snap;
    assert(host_registry_snapshot(&snap));
    assert(snap.host_count == 1);
    assert(strcmp(snap.hosts[0].device_id, "template-001") == 0);
    assert(snap.hosts[0].host_type_id == 999);

    memset(&in, 0, sizeof(in));
    in.msg_type = FLEET_MSG_REPORT;
    strncpy(in.header.device_id, "template-001", sizeof(in.header.device_id) - 1);
    in.header.host_type_id = 999;
    in.header.seq = 2;
    in.header.ts_ms = 3000;
    in.header.status = FLEET_STATUS_READING_VALID;
    in.reading_count = 1;
    in.readings[0].tlv_id = 16;
    in.readings[0].type = FLEET_VAL_FLOAT;
    in.readings[0].value.f32 = 12.5f;
    in.readings[0].valid = true;

    n = fleet_tlv_encode(&in, buf, sizeof(buf));
    assert(n > 0);
    assert(host_registry_ingest_frame_ex(buf, (size_t)n, 4000, 0x1234, false) == 0);

    assert(host_registry_snapshot(&snap));
    assert(snap.hosts[0].readings[0].valid);
    assert(snap.hosts[0].readings[0].value > 12.4 && snap.hosts[0].readings[0].value < 12.6);

    host_registry_refresh_links(4000 + FLEET_HOST_STALE_MS);
    assert(host_registry_snapshot(&snap));
    assert(snap.hosts[0].link_ok);
    assert(snap.joined_count == 1);

    host_registry_refresh_links(4000 + FLEET_HOST_STALE_MS + 1);
    assert(host_registry_snapshot(&snap));
    assert(!snap.hosts[0].link_ok);
    assert(snap.joined_count == 0);

    in.header.host_type_id = 404;
    n = fleet_tlv_encode(&in, buf, sizeof(buf));
    assert(host_registry_ingest_frame_ex(buf, (size_t)n, 5000, 0, false) == -3);

    printf("test_host_registry: OK\n");
    return 0;
}
