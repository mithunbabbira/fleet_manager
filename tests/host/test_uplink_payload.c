#include "uplink_payload.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uplink_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snprintf(snap.device_id, sizeof(snap.device_id), "%s", "fleet-demo-001");
    snprintf(snap.node_id, sizeof(snap.node_id), "%s", "esp32c6-01");
    snprintf(snap.obd_profile, sizeof(snap.obd_profile), "%s", "can_11_500");
    snprintf(snap.obd_protocol, sizeof(snap.obd_protocol), "%s", "unknown");
    snap.uptime_seconds = 29;
    snap.poller_status = "on";
    snap.cmds_ok = 143;
    snap.ts_ms = 1710000000123ULL;

    /* Missing speed: keys omitted (schema rejects null) + speed_ok false. */
    snap.speed.valid = false;

    snap.rpm.valid = true;
    snap.rpm.ok = true;
    snap.rpm.value = 779.5;
    snap.rpm.age_ms = 120;
    snprintf(snap.rpm.raw, sizeof(snap.rpm.raw), "%s", "410C0C2E");

    char buf[2048];
    int n = uplink_payload_build(&snap, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"schemaId\":\"1087\"") != NULL);
    assert(strstr(buf, "null") == NULL);            /* schema forbids null */
    assert(strstr(buf, "\"speed_kmh\"") == NULL);   /* omitted entirely */
    assert(strstr(buf, "\"speed_raw_hex\"") == NULL);
    assert(strstr(buf, "\"speed_age_ms\"") == NULL);
    assert(strstr(buf, "\"speed_ok\":false") != NULL);
    assert(strstr(buf, "\"rpm\":779.5") != NULL);
    assert(strstr(buf, "\"rpm_ok\":true") != NULL);
    assert(strstr(buf, "\"ts_ms\":1710000000123") != NULL);
    assert(strstr(buf, "255") == NULL);
    assert(strstr(buf, "ble_peer_address") == NULL);
    assert(strstr(buf, "adapter_name") == NULL);
    assert(strstr(buf, "ble_connected") == NULL);
    assert(strstr(buf, "elm_ready") == NULL);
    assert(strstr(buf, "ble_reconnects") == NULL);

    /* Stale speed (ok but old) must also be omitted. */
    snap.speed.valid = true;
    snap.speed.ok = true;
    snap.speed.value = 255;
    snap.speed.age_ms = 20000;
    snprintf(snap.speed.raw, sizeof(snap.speed.raw), "%s", "410DFF");
    n = uplink_payload_build(&snap, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"speed_kmh\"") == NULL);
    assert(strstr(buf, "\"speed_ok\":false") != NULL);

    /* Fresh parked speed 0 is valid. */
    snap.speed.age_ms = 100;
    snap.speed.value = 0;
    snprintf(snap.speed.raw, sizeof(snap.speed.raw), "%s", "410D00");
    n = uplink_payload_build(&snap, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"speed_kmh\":0") != NULL);
    assert(strstr(buf, "\"speed_ok\":true") != NULL);

    char payload[1800];
    int pn = uplink_payload_build_payload(&snap, payload, sizeof(payload));
    assert(pn > 0);
    assert(payload[0] == '{');
    assert(strstr(payload, "schemaId") == NULL);

    char event[1900];
    int en = uplink_payload_build_queued_event(payload, 12345ULL, event, sizeof(event));
    assert(en > 0);
    assert(strstr(event, "\"queued_at_ms\":12345") != NULL);
    assert(strstr(event, "\"payload\":{") != NULL);

    char line2[1900];
    assert(uplink_payload_build_queued_event(payload, 67890ULL, line2, sizeof(line2)) > 0);
    char blob[4000];
    snprintf(blob, sizeof(blob), "%s\n%s", event, line2);
    char batch[4500];
    int bn = uplink_payload_build_batch(blob, 2, batch, sizeof(batch));
    assert(bn > 0);
    assert(batch[0] == '[');
    assert(batch[bn - 1] == ']');
    assert(strstr(batch, "\"events\"") == NULL);
    assert(strstr(batch, "\"queued_at_ms\"") == NULL);
    assert(strstr(batch, "{\"schemaId\":\"1087\",\"payload\":{") != NULL);
    /* One schemaId wrapper per queued event. */
    {
        int schema_n = 0;
        for (const char *s = batch; (s = strstr(s, "\"schemaId\":\"1087\"")) != NULL; s += 8) {
            schema_n++;
        }
        assert(schema_n == 2);
    }

    snap.gps_ok = true;
    snap.lat = 12.9716;
    snap.lng = 77.5946;
    n = uplink_payload_build(&snap, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"gps_ok\":true") != NULL);
    assert(strstr(buf, "\"lat\":12.9716") != NULL || strstr(buf, "\"lat\":12.971") != NULL);
    assert(strstr(buf, "\"lng\":77.5946") != NULL || strstr(buf, "\"lng\":77.594") != NULL);
    /* Must be inside payload, not next to schemaId only */
    assert(strstr(buf, "\"payload\":{") != NULL);

    pn = uplink_payload_build_payload(&snap, payload, sizeof(payload));
    assert(pn > 0);
    assert(strstr(payload, "\"gps_ok\":true") != NULL);
    assert(strstr(payload, "\"lat\":") != NULL);
    assert(strstr(payload, "\"lng\":") != NULL);
    assert(uplink_payload_build_queued_event(payload, 1ULL, event, sizeof(event)) > 0);
    bn = uplink_payload_build_batch(event, 1, batch, sizeof(batch));
    assert(bn > 0);
    assert(batch[0] == '[');
    assert(strstr(batch, "\"gps_ok\":true") != NULL);
    assert(strstr(batch, "\"lat\":") != NULL);
    assert(strstr(batch, "\"lng\":") != NULL);
    assert(strstr(batch, "\"source\":\"esp32_obd\"") != NULL);

    snap.gps_ok = false;
    n = uplink_payload_build(&snap, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"gps_ok\":false") != NULL);
    assert(strstr(buf, "\"lat\":") == NULL);
    assert(strstr(buf, "\"lng\":") == NULL);

    assert(uplink_should_enqueue(true, false) == true);
    assert(uplink_should_enqueue(false, true) == true);
    assert(uplink_should_enqueue(true, true) == true);
    assert(uplink_should_enqueue(false, false) == false);

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

    printf("test_uplink_payload: PASS\n");
    return 0;
}
