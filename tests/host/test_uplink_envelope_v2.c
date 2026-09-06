#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "uplink_envelope.h"

int main(void)
{
    char did[40], nid[40];
    uplink_virtual_gps_ids("fleet-demo-001", did, sizeof(did), nid, sizeof(nid));
    assert(strcmp(did, "fleet-demo-001_GPS") == 0);
    assert(strcmp(nid, "node-fleet-demo-001_GPS") == 0);

    char payload[128];
    assert(uplink_build_gps_payload(true, 12.9716, 77.5946, payload, sizeof(payload)) > 0);
    assert(strstr(payload, "\"gps_ok\":true") != NULL);
    assert(strstr(payload, "lat") != NULL);

    char env[512];
    assert(uplink_build_envelope(did, nid, UPLINK_SCHEMA_GPS, "gps", 1710000001000ULL, payload, env,
                                 sizeof(env)) > 0);
    assert(strstr(env, "\"schemaId\":\"1089\"") != NULL);
    assert(strstr(env, "\"device_type\":\"gps\"") != NULL);
    assert(strstr(env, "\"device_id\":\"fleet-demo-001_GPS\"") != NULL);
    assert(strstr(env, "\"payload\":{") != NULL);

    assert(uplink_build_envelope("host-1", "node-h", "1088", NULL, 1, "{}", env, sizeof(env)) > 0);
    assert(strstr(env, "device_type") == NULL);

    assert(uplink_build_envelope("", "n", "1089", "gps", 1, "{}", env, sizeof(env)) < 0);
    assert(uplink_build_envelope("d", "", "1089", "gps", 1, "{}", env, sizeof(env)) < 0);

    assert(uplink_gps_worth_sending(false, 0, 0, 0, 1, 1, 1000));
    assert(!uplink_gps_worth_sending(true, 12.0, 77.0, 1000, 12.0001, 77.0001, 2000));
    assert(uplink_gps_worth_sending(true, 12.0, 77.0, 1000, 12.0, 77.0, 1000 + UPLINK_GPS_ONLY_HEARTBEAT_MS));

    double d = uplink_gps_distance_m(12.0, 77.0, 12.001, 77.0);
    assert(d > 50.0); /* ~111 m per 0.001 deg lat */

    printf("test_uplink_envelope_v2: ok\n");
    return 0;
}
