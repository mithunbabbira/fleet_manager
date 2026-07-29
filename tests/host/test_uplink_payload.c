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
    snprintf(snap.ble_peer_address, sizeof(snap.ble_peer_address), "%s", "46:FC:0D:32:1E:66");
    snprintf(snap.adapter_name, sizeof(snap.adapter_name), "%s", "MODAXE OBDII");
    snprintf(snap.obd_profile, sizeof(snap.obd_profile), "%s", "can_11_500");
    snprintf(snap.obd_protocol, sizeof(snap.obd_protocol), "%s", "unknown");
    snap.uptime_seconds = 29;
    snap.ble_connected = true;
    snap.elm_ready = true;
    snap.poller_status = "on";
    snap.cmds_ok = 143;

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
    assert(strstr(buf, "255") == NULL);

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

    printf("test_uplink_payload: PASS\n");
    return 0;
}
