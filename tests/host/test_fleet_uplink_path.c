#include "fleet_tlv.h"
#include "host_registry.h"
#include "uplink_payload.h"
#include "uplink_schema.h"

#ifdef HOST_REGISTRY_HOST_TEST
#include "telemetry_bus_shim.h"
#else
#include "telemetry_bus.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

esp_err_t telemetry_bus_init(void) { return 0; }
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

/* End-to-end path without radio: TLV REPORT -> registry -> uplink host events. */

int main(void)
{
    host_registry_init();

    fleet_encode_input_t in;
    memset(&in, 0, sizeof(in));
    in.msg_type = FLEET_MSG_REPORT;
    strncpy(in.header.device_id, "ul212-001", sizeof(in.header.device_id) - 1);
    strncpy(in.header.node_id, "node-ul212-001", sizeof(in.header.node_id) - 1);
    strncpy(in.header.schema_id, "1088", sizeof(in.header.schema_id) - 1);
    strncpy(in.header.host_type, "ul212_ble_fetch", sizeof(in.header.host_type) - 1);
    in.header.host_type_id = 1;
    in.header.seq = 1;
    in.header.ts_ms = 5000;
    in.header.status = FLEET_STATUS_READING_VALID | FLEET_STATUS_SENSOR_CONNECTED;
    in.reading_count = 1;
    in.readings[0].tlv_id = 16;
    in.readings[0].type = FLEET_VAL_FLOAT;
    in.readings[0].value.f32 = 40.9f;
    in.readings[0].valid = true;

    uint8_t frame[FLEET_TLV_MAX_FRAME];
    int n = fleet_tlv_encode(&in, frame, sizeof(frame));
    assert(n > 0);
    assert(host_registry_ingest_frame_ex(frame, (size_t)n, 6000, 0x42, false) == 0);

    fleet_registry_snapshot_t reg;
    assert(host_registry_snapshot(&reg));
    assert(reg.host_count == 1);
    assert(strcmp(reg.hosts[0].schema_id, "1088") == 0);
    assert(strcmp(reg.hosts[0].node_id, "node-ul212-001") == 0);

    uplink_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snprintf(snap.device_id, sizeof(snap.device_id), "%s", "carrier-001");
    snprintf(snap.node_id, sizeof(snap.node_id), "%s", "node-001");
    snap.ts_ms = 6000;
    snap.host_count = reg.host_count;
    for (uint8_t i = 0; i < reg.host_count && i < UPLINK_MAX_HOSTS; i++) {
        const fleet_registry_host_t *h = &reg.hosts[i];
        uplink_host_report_t *uh = &snap.hosts[i];
        snprintf(uh->device_id, sizeof(uh->device_id), "%s", h->device_id);
        snprintf(uh->node_id, sizeof(uh->node_id), "%s", h->node_id);
        snprintf(uh->schema_id, sizeof(uh->schema_id), "%s", h->schema_id);
        snprintf(uh->host_type, sizeof(uh->host_type), "%s", h->host_type);
        uh->host_type_id = h->host_type_id;
        uh->ts_ms = h->last_seen_ms;
        uh->reading_count = h->reading_count;
        for (uint8_t r = 0; r < uh->reading_count && r < UPLINK_MAX_HOST_READINGS; r++) {
            snprintf(uh->readings[r].key, sizeof(uh->readings[r].key), "%s", h->readings[r].key);
            snprintf(uh->readings[r].unit, sizeof(uh->readings[r].unit), "%s", h->readings[r].unit);
            uh->readings[r].value = h->readings[r].value;
            uh->readings[r].valid = h->readings[r].valid;
        }
    }

    uplink_emit_ctx_t emit = {
        .include_obd = false,
        .include_gps = false,
        .obd_schema_id = NULL,
    };
    uplink_event_t events[UPLINK_MAX_EVENTS_PER_TICK];
    uint8_t count = 0;
    assert(uplink_events_from_snapshot(&snap, &emit, events, UPLINK_MAX_EVENTS_PER_TICK,
                                       &count) == 0);
    assert(count >= 1);

    char buf[2048];
    int jn = uplink_events_serialize_live(events, count, buf, sizeof(buf));
    assert(jn > 0);
    assert(strstr(buf, "\"schemaId\":\"1088\"") != NULL);
    assert(strstr(buf, "\"height_mm\":") != NULL);
    assert(strstr(buf, "\"host_type\":\"ul212_ble_fetch\"") != NULL);
    assert(strstr(buf, "\"device_id\":\"ul212-001\"") != NULL);
    assert(strstr(buf, "\"node_id\":\"node-ul212-001\"") != NULL);
    assert(reg.hosts[0].readings[0].valid);
    assert(reg.hosts[0].readings[0].value > 40.8 && reg.hosts[0].readings[0].value < 41.0);
    assert(strstr(buf, "\"key\":\"height_mm\"") == NULL);
    assert(strstr(buf, "\"hosts\":[") == NULL);
    assert(uplink_schema_for_host(1) == NULL);

    printf("test_fleet_uplink_path: PASS\n");
    return 0;
}
