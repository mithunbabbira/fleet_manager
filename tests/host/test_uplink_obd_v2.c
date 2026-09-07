#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "uplink_obd.h"

int main(void)
{
    uplink_obd_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snprintf(snap.obd_profile, sizeof(snap.obd_profile), "%s", "mode01_core");
    snprintf(snap.obd_protocol, sizeof(snap.obd_protocol), "%s", "ISO15765-4 CAN11/500");
    snap.uptime_seconds = 42;
    snap.poller_status = "on";
    snap.cmds_ok = 10;
    snap.cmds_fail = 1;

    snap.rpm.valid = true;
    snap.rpm.ok = true;
    snap.rpm.value = 1500.0;
    snap.rpm.age_ms = 100;
    snprintf(snap.rpm.raw, sizeof(snap.rpm.raw), "%s", "410C0BB8");

    snap.speed.valid = true;
    snap.speed.ok = true;
    snap.speed.value = 60.0;
    snap.speed.age_ms = 200;
    snprintf(snap.speed.raw, sizeof(snap.speed.raw), "%s", "410D3C");

    assert(uplink_obd_has_fresh_pid(&snap));
    assert(uplink_pid_is_fresh_ok(&snap.rpm));

    char buf[640];
    int n = uplink_build_obd_payload(&snap, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"obd_profile\":\"mode01_core\"") != NULL);
    assert(strstr(buf, "\"rpm\":1500") != NULL);
    assert(strstr(buf, "\"rpm_ok\":true") != NULL);
    assert(strstr(buf, "\"speed_kmh\":60") != NULL);
    assert(strstr(buf, "\"coolant_ok\":false") != NULL);
    assert(strstr(buf, "\"throttle_ok\":false") != NULL);
    assert(strstr(buf, "\"voltage_ok\":false") != NULL);
    assert(strstr(buf, "\"source\":\"esp32_obd\"") != NULL);

    /* Stale PID must not emit numeric field */
    snap.rpm.age_ms = UPLINK_PID_FRESH_MS + 1;
    assert(!uplink_pid_is_fresh_ok(&snap.rpm));
    assert(uplink_obd_has_fresh_pid(&snap)); /* speed still fresh */
    n = uplink_build_obd_payload(&snap, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"rpm_ok\":false") != NULL);
    assert(strstr(buf, "\"rpm\":") == NULL);

    snap.speed.age_ms = UPLINK_PID_FRESH_MS + 1;
    assert(!uplink_obd_has_fresh_pid(&snap));

    assert(strcmp(UPLINK_SCHEMA_OBD, "1087") == 0);

    printf("test_uplink_obd_v2: ok\n");
    return 0;
}
