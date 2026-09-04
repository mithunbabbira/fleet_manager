#include "fleet_tlv.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    fleet_encode_input_t in;
    memset(&in, 0, sizeof(in));
    in.msg_type = FLEET_MSG_REPORT;
    strncpy(in.header.device_id, "ul212-001", sizeof(in.header.device_id) - 1);
    in.header.host_type_id = 1;
    in.header.seq = 7;
    in.header.ts_ms = 1710000000123ULL;
    in.header.status = FLEET_STATUS_SENSOR_CONNECTED | FLEET_STATUS_READING_VALID;
    in.header.manifest_version = 1;

    in.reading_count = 2;
    in.readings[0].tlv_id = 16;
    in.readings[0].type = FLEET_VAL_FLOAT;
    in.readings[0].value.f32 = 40.9f;
    in.readings[0].valid = true;
    in.readings[1].tlv_id = 19;
    in.readings[1].type = FLEET_VAL_UINT8;
    in.readings[1].value.u8 = 90;
    in.readings[1].valid = true;

    uint8_t buf[FLEET_TLV_MAX_FRAME];
    int n = fleet_tlv_encode(&in, buf, sizeof(buf));
    assert(n > 0);

    fleet_decoded_frame_t out;
    memset(&out, 0, sizeof(out));
    assert(fleet_tlv_decode(buf, (size_t)n, &out) == 0);
    assert(out.msg_type == FLEET_MSG_REPORT);
    assert(strcmp(out.header.device_id, "ul212-001") == 0);
    assert(out.header.host_type_id == 1);
    assert(out.header.seq == 7);
    assert(out.reading_count == 2);

    /* Envelope fields round-trip. */
    memset(&in, 0, sizeof(in));
    in.msg_type = FLEET_MSG_HELLO;
    strncpy(in.header.device_id, "ul212-001", sizeof(in.header.device_id) - 1);
    strncpy(in.header.node_id, "node-ul212-001", sizeof(in.header.node_id) - 1);
    strncpy(in.header.schema_id, "1088", sizeof(in.header.schema_id) - 1);
    strncpy(in.header.host_type, "ul212_ble_fetch", sizeof(in.header.host_type) - 1);
    in.header.host_type_id = 1;
    in.header.seq = 1;
    in.reading_count = 1;
    in.readings[0].tlv_id = FLEET_TLV_METRIC_MAP;
    in.readings[0].type = FLEET_VAL_STRING;
    in.readings[0].valid = true;
    strncpy(in.readings[0].value.str, "16:height_mm:mm:f;19:signal::u8",
            sizeof(in.readings[0].value.str) - 1);
    n = fleet_tlv_encode(&in, buf, sizeof(buf));
    assert(n > 0);
    assert(fleet_tlv_decode(buf, (size_t)n, &out) == 0);
    assert(fleet_tlv_header_has_envelope(&out.header));
    assert(strcmp(out.header.node_id, "node-ul212-001") == 0);
    assert(strcmp(out.header.schema_id, "1088") == 0);
    assert(out.reading_count == 1);
    assert(out.readings[0].tlv_id == FLEET_TLV_METRIC_MAP);

    uint16_t crc = fleet_tlv_crc16(buf, (size_t)n - 2);
    uint16_t got = (uint16_t)(buf[n - 2] | ((uint16_t)buf[n - 1] << 8));
    assert(crc == got);

    buf[10] ^= 0x01;
    assert(fleet_tlv_decode(buf, (size_t)n, &out) != 0);

    printf("test_fleet_tlv: OK\n");
    return 0;
}
