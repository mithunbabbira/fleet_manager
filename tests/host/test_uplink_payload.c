#include "uplink_payload.h"
#include "uplink_schema.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void fill_base_snapshot(uplink_snapshot_t *snap)
{
    memset(snap, 0, sizeof(*snap));
    snprintf(snap->device_id, sizeof(snap->device_id), "%s", "carrier-042");
    snprintf(snap->node_id, sizeof(snap->node_id), "%s", "node-042");
    snprintf(snap->obd_profile, sizeof(snap->obd_profile), "%s", "fleet_basic");
    snprintf(snap->obd_protocol, sizeof(snap->obd_protocol), "%s", "ISO15765-4 CAN11/500");
    snap->uptime_seconds = 3605;
    snap->poller_status = "on";
    snap->cmds_ok = 1440;
    snap->ts_ms = 1710000001000ULL;

    snap->rpm.valid = true;
    snap->rpm.ok = true;
    snap->rpm.value = 790.0;
    snap->rpm.age_ms = 80;
    snprintf(snap->rpm.raw, sizeof(snap->rpm.raw), "%s", "410C0C31");
}

int main(void)
{
    uplink_snapshot_t snap;
    fill_base_snapshot(&snap);

  /* Missing speed: keys omitted + speed_ok false in OBD payload. */
    snap.speed.valid = false;

    uplink_emit_ctx_t emit = {
        .include_obd = true,
        .include_gps = false,
        .obd_schema_id = NULL,
    };
    uplink_event_t events[UPLINK_MAX_EVENTS_PER_TICK];
    uint8_t count = 0;
    assert(uplink_events_from_snapshot(&snap, &emit, events, UPLINK_MAX_EVENTS_PER_TICK,
                                       &count) == 0);
    assert(count == 1);

    char buf[2048];
    int n = uplink_events_serialize_live(events, count, buf, sizeof(buf));
    assert(n > 0);
    assert(buf[0] == '{');
    assert(strstr(buf, "\"device_id\":\"carrier-042\"") != NULL);
    assert(strstr(buf, "\"node_id\":\"node-042\"") != NULL);
    assert(strstr(buf, "\"schemaId\":\"1087\"") != NULL);
    assert(strstr(buf, "\"ts_ms\":1710000001000") != NULL);
    assert(strstr(buf, "\"payload\":{") != NULL);
    /* Mandatory envelope: never inside payload only */
    assert(strstr(buf, "\"schema_version\"") == NULL);
    assert(strstr(buf, "null") == NULL);
    assert(strstr(buf, "\"speed_kmh\"") == NULL);
    assert(strstr(buf, "\"speed_ok\":false") != NULL);
    assert(strstr(buf, "\"rpm\":790") != NULL);
    assert(strstr(buf, "\"rpm_ok\":true") != NULL);
    assert(strstr(buf, "\"obd_profile\":\"fleet_basic\"") != NULL);

    /* Stale speed omitted. */
    snap.speed.valid = true;
    snap.speed.ok = true;
    snap.speed.value = 255;
    snap.speed.age_ms = 20000;
    snprintf(snap.speed.raw, sizeof(snap.speed.raw), "%s", "410DFF");
    assert(uplink_events_from_snapshot(&snap, &emit, events, UPLINK_MAX_EVENTS_PER_TICK,
                                       &count) == 0);
    n = uplink_events_serialize_live(events, count, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"speed_kmh\"") == NULL);
    assert(strstr(buf, "\"speed_ok\":false") != NULL);

    /* Fresh parked speed 0 is valid. */
    snap.speed.age_ms = 100;
    snap.speed.value = 0;
    snprintf(snap.speed.raw, sizeof(snap.speed.raw), "%s", "410D00");
    assert(uplink_events_from_snapshot(&snap, &emit, events, UPLINK_MAX_EVENTS_PER_TICK,
                                       &count) == 0);
    n = uplink_events_serialize_live(events, count, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"speed_kmh\":0") != NULL);
    assert(strstr(buf, "\"speed_ok\":true") != NULL);

    char queued[768];
    int qn = uplink_event_serialize_queued(&events[0], 12345ULL, queued, sizeof(queued));
    assert(qn > 0);
    assert(strstr(queued, "\"queued_at_ms\":12345") != NULL);
    assert(strstr(queued, "\"schemaId\":\"1087\"") != NULL);

    char queued2[768];
    assert(uplink_event_serialize_queued(&events[0], 67890ULL, queued2, sizeof(queued2)) > 0);
    char blob[2000];
    snprintf(blob, sizeof(blob), "%s\n%s", queued, queued2);
    char batch[2500];
    int bn = uplink_payload_build_batch(blob, 2, batch, sizeof(batch));
    assert(bn > 0);
    assert(batch[0] == '[');
    assert(batch[bn - 1] == ']');
    assert(strstr(batch, "\"queued_at_ms\"") == NULL);
    assert(strstr(batch, "\"device_id\":\"carrier-042\"") != NULL);
    {
        int schema_n = 0;
        for (const char *s = batch; (s = strstr(s, "\"schemaId\":\"1087\"")) != NULL; s += 8) {
            schema_n++;
        }
        assert(schema_n == 2);
    }

    /* GPS as separate virtual-device event. */
    snap.gps_ok = true;
    snap.lat = 12.9716;
    snap.lng = 77.5946;
    emit.include_gps = true;
    assert(uplink_events_from_snapshot(&snap, &emit, events, UPLINK_MAX_EVENTS_PER_TICK,
                                       &count) == 0);
    assert(count == 2);
    n = uplink_events_serialize_live(events, count, buf, sizeof(buf));
    assert(n > 0);
    assert(buf[0] == '[');
    assert(strstr(buf, "\"schemaId\":\"1089\"") != NULL);
    assert(strstr(buf, "\"device_id\":\"gps-042\"") != NULL);
    assert(strstr(buf, "\"node_id\":\"node-gps-042\"") != NULL);
    assert(strstr(buf, "\"lat\":12.9716") != NULL || strstr(buf, "\"lat\":12.971") != NULL);
    assert(strstr(buf, "\"lng\":77.5946") != NULL || strstr(buf, "\"lng\":77.594") != NULL);
    assert(strstr(buf, "\"source\":\"esp32_obd\"") != NULL);

    /* Single GPS-only event is a bare object, not an array. */
    emit.include_obd = false;
    assert(uplink_events_from_snapshot(&snap, &emit, events, UPLINK_MAX_EVENTS_PER_TICK,
                                       &count) == 0);
    assert(count == 1);
    n = uplink_events_serialize_live(events, count, buf, sizeof(buf));
    assert(n > 0);
    assert(buf[0] == '{');
    assert(buf[0] != '[');

    /* Host readings split into one event per valid reading. */
    fill_base_snapshot(&snap);
    snap.gps_ok = false;
    emit.include_obd = false;
    emit.include_gps = false;
    snap.host_count = 1;
    snprintf(snap.hosts[0].device_id, sizeof(snap.hosts[0].device_id), "%s", "ul212-001");
    snprintf(snap.hosts[0].host_type, sizeof(snap.hosts[0].host_type), "%s", "ul212_ble_fetch");
    snap.hosts[0].host_type_id = 1;
    snap.hosts[0].ts_ms = 1710000001100ULL;
    snap.hosts[0].reading_count = 2;
    snprintf(snap.hosts[0].readings[0].key, sizeof(snap.hosts[0].readings[0].key), "%s",
             "height_mm");
    snap.hosts[0].readings[0].value = 40.9;
    snprintf(snap.hosts[0].readings[0].unit, sizeof(snap.hosts[0].readings[0].unit), "%s", "mm");
    snap.hosts[0].readings[0].valid = true;
    snprintf(snap.hosts[0].readings[1].key, sizeof(snap.hosts[0].readings[1].key), "%s",
             "signal");
    snap.hosts[0].readings[1].value = 85;
    snprintf(snap.hosts[0].readings[1].unit, sizeof(snap.hosts[0].readings[1].unit), "%s", "");
    snap.hosts[0].readings[1].valid = false;
    assert(uplink_events_from_snapshot(&snap, &emit, events, UPLINK_MAX_EVENTS_PER_TICK,
                                       &count) == 0);
    assert(count == 1);
    n = uplink_events_serialize_live(events, count, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"schemaId\":\"1088\"") != NULL);
    assert(strstr(buf, "\"device_id\":\"ul212-001\"") != NULL);
    assert(strstr(buf, "\"node_id\":\"node-ul212-001\"") != NULL);
    assert(strstr(buf, "\"height_mm\"") != NULL);
    assert(strstr(buf, "\"hosts\":[") == NULL);

    assert(uplink_should_enqueue(true, false) == true);
    assert(uplink_should_enqueue(false, true) == true);
    assert(uplink_tick_worth_producing(false, false, false, &snap) == true);

    {
        double d = uplink_gps_distance_m(12.9716, 77.5946, 12.9721, 77.5946);
        assert(d > 50.0);
        assert(d < 80.0);
        assert(uplink_gps_only_worth_sending(false, 0, 0, 0, 12.9716, 77.5946, 1000) == true);
        assert(uplink_gps_only_worth_sending(true, 12.9716, 77.5946, 1000, 12.9716, 77.5946,
                                             2000) == false);
        assert(uplink_gps_only_worth_sending(true, 12.9716, 77.5946, 1000, 12.9716, 77.5946,
                                             1000 + UPLINK_GPS_ONLY_HEARTBEAT_MS) == true);
        assert(uplink_gps_only_worth_sending(true, 12.9716, 77.5946, 1000, 12.9721, 77.5946,
                                             2000) == true);
    }

    char gps_dev[40];
    char gps_node[40];
    uplink_virtual_gps_ids("carrier-042", "node-042", gps_dev, sizeof(gps_dev), gps_node,
                           sizeof(gps_node));
    assert(strcmp(gps_dev, "gps-042") == 0);
    assert(strcmp(gps_node, "node-gps-042") == 0);

    uplink_event_t bad = events[0];
    bad.device_id[0] = '\0';
    assert(uplink_event_serialize(&bad, buf, sizeof(buf)) < 0);

    printf("test_uplink_payload: PASS\n");
    return 0;
}
