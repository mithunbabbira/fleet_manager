/* End-to-end: a brand-new Zigbee host joins (TLV HELLO+REPORT) and its
 * reading ends up correctly queued as a batch entry, ready to upload.
 *
 * This mirrors collect_hosts() in components/uplink/uplink.c line for
 * line (host_registry_snapshot -> per-host uplink_host_report_t ->
 * uplink_build_host_report_payload -> uplink_build_envelope ->
 * uplink_batch_add) using the exact same production functions, since
 * collect_hosts() itself lives in uplink.c and pulls in lte_time_now_ms()
 * and other ESP-IDF-coupled pieces that aren't host-testable directly.
 */
#include "fleet_tlv.h"
#include "host_registry.h"
#include "uplink_batch.h"
#include "uplink_envelope.h"
#include "uplink_host.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Ingest one HELLO (join) + REPORT (first reading) pair for a new device. */
static void join_device(const char *device_id, const char *node_id, const char *schema_id,
                        const char *host_type, const char *metric_map, uint16_t tlv_id,
                        float value, uint64_t hello_ts, uint64_t report_ts)
{
    fleet_encode_input_t in;
    uint8_t buf[FLEET_TLV_MAX_FRAME];

    memset(&in, 0, sizeof(in));
    in.msg_type = FLEET_MSG_HELLO;
    strncpy(in.header.device_id, device_id, sizeof(in.header.device_id) - 1);
    strncpy(in.header.node_id, node_id, sizeof(in.header.node_id) - 1);
    strncpy(in.header.schema_id, schema_id, sizeof(in.header.schema_id) - 1);
    strncpy(in.header.host_type, host_type, sizeof(in.header.host_type) - 1);
    in.header.host_type_id = 0; /* dynamic/unknown type, joins via envelope */
    in.header.seq = 1;
    in.header.ts_ms = hello_ts;
    in.reading_count = 1;
    in.readings[0].tlv_id = FLEET_TLV_METRIC_MAP;
    in.readings[0].type = FLEET_VAL_STRING;
    in.readings[0].valid = true;
    strncpy(in.readings[0].value.str, metric_map, sizeof(in.readings[0].value.str) - 1);
    int n = fleet_tlv_encode(&in, buf, sizeof(buf));
    assert(n > 0);
    assert(host_registry_ingest_frame_ex(buf, (size_t)n, hello_ts, 0x1, false) == 0);

    memset(&in, 0, sizeof(in));
    in.msg_type = FLEET_MSG_REPORT;
    strncpy(in.header.device_id, device_id, sizeof(in.header.device_id) - 1);
    strncpy(in.header.node_id, node_id, sizeof(in.header.node_id) - 1);
    strncpy(in.header.schema_id, schema_id, sizeof(in.header.schema_id) - 1);
    in.header.host_type_id = 0;
    in.header.seq = 2;
    in.header.ts_ms = report_ts;
    in.reading_count = 1;
    in.readings[0].tlv_id = tlv_id;
    in.readings[0].type = FLEET_VAL_FLOAT;
    in.readings[0].value.f32 = value;
    in.readings[0].valid = true;
    n = fleet_tlv_encode(&in, buf, sizeof(buf));
    assert(n > 0);
    assert(host_registry_ingest_frame_ex(buf, (size_t)n, report_ts, 0x1, false) == 0);
}

/** collect_hosts()'s per-host conversion (uplink.c), reproduced exactly so
 * this test exercises the real production functions on both sides of it. */
static void collect_hosts_into(uplink_batch_t *batch, uint64_t ts)
{
    fleet_registry_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    assert(host_registry_snapshot(&snap));

    for (uint8_t i = 0; i < snap.host_count && i < FLEET_REGISTRY_MAX_HOSTS; i++) {
        const fleet_registry_host_t *h = &snap.hosts[i];
        if (h->device_id[0] == '\0' || h->schema_id[0] == '\0' || h->node_id[0] == '\0') {
            continue;
        }

        uplink_host_report_t report;
        memset(&report, 0, sizeof(report));
        snprintf(report.device_id, sizeof(report.device_id), "%s", h->device_id);
        snprintf(report.node_id, sizeof(report.node_id), "%s", h->node_id);
        snprintf(report.schema_id, sizeof(report.schema_id), "%s", h->schema_id);
        snprintf(report.host_type, sizeof(report.host_type), "%s", h->host_type);
        report.reading_count = h->reading_count;
        if (report.reading_count > UPLINK_HOST_MAX_READINGS) {
            report.reading_count = UPLINK_HOST_MAX_READINGS;
        }
        for (uint8_t r = 0; r < report.reading_count; r++) {
            snprintf(report.readings[r].key, sizeof(report.readings[r].key), "%s",
                     h->readings[r].key);
            snprintf(report.readings[r].unit, sizeof(report.readings[r].unit), "%s",
                     h->readings[r].unit);
            report.readings[r].value = h->readings[r].value;
            report.readings[r].valid = h->readings[r].valid;
        }
        if (!uplink_host_has_valid_reading(&report)) {
            continue;
        }

        char payload[512];
        char env[UPLINK_BATCH_ENV_MAX];
        if (uplink_build_host_report_payload(&report, payload, sizeof(payload)) < 0) {
            continue;
        }
        if (uplink_build_envelope(report.device_id, report.node_id, report.schema_id, NULL, ts,
                                  payload, env, sizeof(env)) < 0) {
            continue;
        }
        (void)uplink_batch_add(batch, env);
    }
}

int main(void)
{
    host_registry_init();

    /* A brand-new device joins and reports one reading. */
    join_device("newdev-001", "node-newdev-001", "1099", "custom_sensor",
                "30:level_mm:mm:f", 30, 55.5f, 1000, 1100);

    uplink_batch_t batch;
    uplink_batch_clear(&batch);
    collect_hosts_into(&batch, 1710000001000ULL);

    assert(batch.count == 1);
    char out[2048];
    int n = uplink_batch_build_array(&batch, out, sizeof(out));
    assert(n > 0);
    assert(out[0] == '[' && out[n - 1] == ']');
    assert(strstr(out, "\"schemaId\":\"1099\"") != NULL);
    assert(strstr(out, "\"device_id\":\"newdev-001\"") != NULL);
    assert(strstr(out, "\"node_id\":\"node-newdev-001\"") != NULL);
    assert(strstr(out, "\"level_mm\":55.5") != NULL);

    /* A second, different new device joins in the same tick — both must
     * land in the same batch, independently addressable. */
    join_device("newdev-002", "node-newdev-002", "1098", "another_sensor",
                "40:pressure_kpa:kpa:f", 40, 101.3f, 2000, 2100);

    uplink_batch_clear(&batch);
    collect_hosts_into(&batch, 1710000002000ULL);
    assert(batch.count == 2);
    n = uplink_batch_build_array(&batch, out, sizeof(out));
    assert(n > 0);
    assert(strstr(out, "\"device_id\":\"newdev-001\"") != NULL);
    assert(strstr(out, "\"device_id\":\"newdev-002\"") != NULL);
    assert(strstr(out, "\"pressure_kpa\":101.3") != NULL);

    /* A device that HELLOs but never sends a valid reading must not
     * produce a queued entry at all (uplink_host_has_valid_reading gate). */
    fleet_encode_input_t in;
    uint8_t buf[FLEET_TLV_MAX_FRAME];
    memset(&in, 0, sizeof(in));
    in.msg_type = FLEET_MSG_HELLO;
    strncpy(in.header.device_id, "silent-001", sizeof(in.header.device_id) - 1);
    strncpy(in.header.node_id, "node-silent-001", sizeof(in.header.node_id) - 1);
    strncpy(in.header.schema_id, "1097", sizeof(in.header.schema_id) - 1);
    in.header.host_type_id = 0;
    in.header.seq = 1;
    in.header.ts_ms = 3000;
    in.reading_count = 1;
    in.readings[0].tlv_id = FLEET_TLV_METRIC_MAP;
    in.readings[0].type = FLEET_VAL_STRING;
    in.readings[0].valid = true;
    strncpy(in.readings[0].value.str, "50:noise_db:db:f", sizeof(in.readings[0].value.str) - 1);
    int hn = fleet_tlv_encode(&in, buf, sizeof(buf));
    assert(hn > 0);
    assert(host_registry_ingest_frame_ex(buf, (size_t)hn, 3000, 0x1, false) == 0);

    uplink_batch_clear(&batch);
    collect_hosts_into(&batch, 1710000003000ULL);
    /* Still just the two devices that actually reported a value. */
    assert(batch.count == 2);
    n = uplink_batch_build_array(&batch, out, sizeof(out));
    assert(strstr(out, "silent-001") == NULL);

    printf("test_uplink_zigbee_join_v2: OK\n");
    return 0;
}
