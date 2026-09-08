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

    /* --- Decode hardening: malformed/short/corrupt input must fail closed --- */
    assert(fleet_tlv_decode(NULL, 0, &out) != 0);
    assert(fleet_tlv_decode(buf, (size_t)n, NULL) != 0);

    uint8_t too_short[5] = {FLEET_TLV_MAGIC, FLEET_TLV_VERSION, 0, 0, 0};
    assert(fleet_tlv_decode(too_short, sizeof(too_short), &out) != 0);

    uint8_t bad_magic[8] = {0x00, FLEET_TLV_VERSION, 0, 0, 0, 0, 0, 0};
    assert(fleet_tlv_decode(bad_magic, sizeof(bad_magic), &out) != 0);

    uint8_t bad_version[8] = {FLEET_TLV_MAGIC, 0xFF, 0, 0, 0, 0, 0, 0};
    assert(fleet_tlv_decode(bad_version, sizeof(bad_version), &out) != 0);

    /* hdr_len claims more bytes than the buffer actually has. */
    uint8_t hdr_overflow[8] = {FLEET_TLV_MAGIC, FLEET_TLV_VERSION, FLEET_MSG_HELLO, 200,
                              0, 0, 0, 0};
    assert(fleet_tlv_decode(hdr_overflow, sizeof(hdr_overflow), &out) != 0);

    /* A TLV inside the header block claims a value length past the block end.
     * Recompute the CRC over the corrupted bytes so this exercises the
     * off+vlen bounds check specifically, not the CRC-mismatch path above. */
    memset(&in, 0, sizeof(in));
    in.msg_type = FLEET_MSG_HELLO;
    strncpy(in.header.device_id, "trunc-001", sizeof(in.header.device_id) - 1);
    in.header.host_type_id = 1;
    n = fleet_tlv_encode(&in, buf, sizeof(buf));
    assert(n > 0);
    uint8_t hdr_len = buf[3];
    /* STATUS is always the last header TLV when node_id/schema_id/host_type
     * and manifest_version are unset, as here; its layout is
     * [id][type][vlen][value], so vlen sits 2 bytes before the block end. */
    size_t vlen_idx = 4 + hdr_len - 2;
    buf[vlen_idx] = 250; /* claims far more bytes than remain in the header block */
    uint16_t fixup_crc = fleet_tlv_crc16(buf, (size_t)n - 2);
    buf[n - 2] = (uint8_t)(fixup_crc & 0xFF);
    buf[n - 1] = (uint8_t)((fixup_crc >> 8) & 0xFF);
    assert(fleet_tlv_decode(buf, (size_t)n, &out) != 0);

    /* --- Body readings beyond FLEET_TLV_MAX_READINGS are capped, not fatal ---
     * fleet_encode_input_t's readings[] array is itself sized to the cap, so
     * exceeding it requires hand-building the body TLVs onto a real encoded
     * header (mirrors fleet_tlv.c's own wire format: id, type, vlen, value). */
    memset(&in, 0, sizeof(in));
    in.msg_type = FLEET_MSG_REPORT;
    strncpy(in.header.device_id, "overflow-001", sizeof(in.header.device_id) - 1);
    in.header.host_type_id = 1;
    n = fleet_tlv_encode(&in, buf, sizeof(buf)); /* header only, reading_count == 0 */
    assert(n > 0);
    uint8_t ov_hdr_len = buf[3];

    uint8_t big[FLEET_TLV_MAX_FRAME];
    size_t off = 0;
    big[off++] = FLEET_TLV_MAGIC;
    big[off++] = FLEET_TLV_VERSION;
    big[off++] = (uint8_t)FLEET_MSG_REPORT;
    big[off++] = ov_hdr_len;
    memcpy(big + off, buf + 4, ov_hdr_len);
    off += ov_hdr_len;

    const int extra_readings = FLEET_TLV_MAX_READINGS + 4; /* 20 > cap of 16 */
    for (int i = 0; i < extra_readings; i++) {
        big[off++] = (uint8_t)(200 + i); /* tlv id */
        big[off++] = (uint8_t)FLEET_VAL_UINT8;
        big[off++] = 1; /* vlen */
        big[off++] = (uint8_t)i; /* value */
    }

    uint16_t ov_crc = fleet_tlv_crc16(big, off);
    big[off++] = (uint8_t)(ov_crc & 0xFF);
    big[off++] = (uint8_t)((ov_crc >> 8) & 0xFF);

    assert(fleet_tlv_decode(big, off, &out) == 0);
    assert(out.reading_count == FLEET_TLV_MAX_READINGS);

    /* --- fleet_tlv_header_valid: legacy vs. dynamic (envelope) hosts --- */
    fleet_frame_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    assert(!fleet_tlv_header_valid(&hdr)); /* no device_id */

    strncpy(hdr.device_id, "dev-1", sizeof(hdr.device_id) - 1);
    hdr.host_type_id = 1;
    assert(fleet_tlv_header_valid(&hdr)); /* known host_type_id is enough */

    hdr.host_type_id = 0;
    assert(!fleet_tlv_header_valid(&hdr)); /* unknown type, no envelope */

    strncpy(hdr.node_id, "node-1", sizeof(hdr.node_id) - 1);
    strncpy(hdr.schema_id, "1099", sizeof(hdr.schema_id) - 1);
    assert(fleet_tlv_header_valid(&hdr)); /* envelope makes a dynamic host valid */

    printf("test_fleet_tlv_v2: OK\n");
    return 0;
}
