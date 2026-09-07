#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "uplink_host.h"

int main(void)
{
    uplink_host_report_t host;
    memset(&host, 0, sizeof(host));
    snprintf(host.device_id, sizeof(host.device_id), "%s", "ul212-001");
    snprintf(host.node_id, sizeof(host.node_id), "%s", "node-ul212-001");
    snprintf(host.schema_id, sizeof(host.schema_id), "%s", "1088");
    snprintf(host.host_type, sizeof(host.host_type), "%s", "ul212_ble_fetch");
    host.reading_count = 4;

    snprintf(host.readings[0].key, sizeof(host.readings[0].key), "%s", "height_mm");
    host.readings[0].value = 40.9;
    snprintf(host.readings[0].unit, sizeof(host.readings[0].unit), "%s", "mm");
    host.readings[0].valid = true;

    snprintf(host.readings[1].key, sizeof(host.readings[1].key), "%s", "temperature_c");
    host.readings[1].value = 33.2;
    snprintf(host.readings[1].unit, sizeof(host.readings[1].unit), "%s", "C");
    host.readings[1].valid = true;

    snprintf(host.readings[2].key, sizeof(host.readings[2].key), "%s", "tilt_deg");
    host.readings[2].value = 3;
    host.readings[2].unit[0] = '\0';
    host.readings[2].valid = true;

    snprintf(host.readings[3].key, sizeof(host.readings[3].key), "%s", "signal");
    host.readings[3].value = 85;
    host.readings[3].unit[0] = '\0';
    host.readings[3].valid = false; /* omitted */

    assert(uplink_host_has_valid_reading(&host));

    char buf[512];
    int n = uplink_build_host_report_payload(&host, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"host_type\":\"ul212_ble_fetch\"") != NULL);
    assert(strstr(buf, "\"height_mm\":40.9") != NULL);
    assert(strstr(buf, "\"height_mm_unit\":\"mm\"") != NULL);
    assert(strstr(buf, "\"temperature_c\":33.2") != NULL);
    assert(strstr(buf, "\"temperature_c_unit\":\"C\"") != NULL);
    assert(strstr(buf, "\"tilt_deg\":3") != NULL);
    assert(strstr(buf, "\"tilt_deg_unit\"") == NULL);
    assert(strstr(buf, "\"signal\":") == NULL);
    assert(strstr(buf, "\"key\":\"height_mm\"") == NULL);

    /* No valid readings → fail */
    host.readings[0].valid = false;
    host.readings[1].valid = false;
    host.readings[2].valid = false;
    assert(!uplink_host_has_valid_reading(&host));
    assert(uplink_build_host_report_payload(&host, buf, sizeof(buf)) < 0);

    assert(strcmp(UPLINK_SCHEMA_HOST, "1088") == 0);

    printf("test_uplink_host_v2: ok\n");
    return 0;
}
