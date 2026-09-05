#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLEET_TLV_MAGIC 0x46u
#define FLEET_TLV_VERSION 1u
/** Zigbee custom-cluster path allows ~240 B; HELLO metric map needs headroom. */
#define FLEET_TLV_MAX_FRAME 240u
#define FLEET_TLV_MAX_READINGS 16u
#define FLEET_DEVICE_ID_MAX 32u
#define FLEET_NODE_ID_MAX 40u
#define FLEET_SCHEMA_ID_MAX 16u
#define FLEET_HOST_TYPE_NAME_MAX 32u
#define FLEET_METRIC_MAP_MAX 160u

typedef enum {
    FLEET_MSG_HELLO = 1,
    FLEET_MSG_REPORT = 2,
    FLEET_MSG_ACK = 3,
    FLEET_MSG_NACK = 4,
} fleet_msg_type_t;

typedef enum {
    FLEET_TLV_DEVICE_ID = 1,
    FLEET_TLV_HOST_TYPE_ID = 2,
    FLEET_TLV_SEQ = 3,
    FLEET_TLV_TS_MS = 4,
    FLEET_TLV_STATUS = 5,
    FLEET_TLV_MANIFEST_VER = 6,
    FLEET_TLV_TS_MS_HI = 7,
    FLEET_TLV_NODE_ID = 8,
    FLEET_TLV_SCHEMA_ID = 9,
    FLEET_TLV_HOST_TYPE = 10,
    /** Body (HELLO): `tlv_id:key:unit:type;...` — see fleet_tlv docs. */
    FLEET_TLV_METRIC_MAP = 11,
} fleet_header_tlv_id_t;

typedef enum {
    FLEET_VAL_FLOAT = 1,
    FLEET_VAL_UINT8 = 2,
    FLEET_VAL_UINT16 = 3,
    FLEET_VAL_INT32 = 4,
    FLEET_VAL_STRING = 5,
} fleet_value_type_t;

typedef enum {
    FLEET_STATUS_SENSOR_CONNECTED = 1u << 0,
    FLEET_STATUS_READING_VALID = 1u << 1,
} fleet_status_flag_t;

typedef struct {
    char device_id[FLEET_DEVICE_ID_MAX];
    char node_id[FLEET_NODE_ID_MAX];
    char schema_id[FLEET_SCHEMA_ID_MAX];
    char host_type[FLEET_HOST_TYPE_NAME_MAX];
    uint16_t host_type_id;
    uint8_t seq;
    uint64_t ts_ms;
    uint8_t status;
    uint8_t manifest_version;
} fleet_frame_header_t;

typedef struct {
    uint16_t tlv_id;
    fleet_value_type_t type;
    bool valid;
    union {
        float f32;
        uint8_t u8;
        uint16_t u16;
        int32_t i32;
        char str[FLEET_METRIC_MAP_MAX];
    } value;
} fleet_tlv_value_t;

typedef struct {
    fleet_msg_type_t msg_type;
    fleet_frame_header_t header;
    fleet_tlv_value_t readings[FLEET_TLV_MAX_READINGS];
    uint8_t reading_count;
} fleet_decoded_frame_t;

typedef struct {
    fleet_msg_type_t msg_type;
    fleet_frame_header_t header;
    fleet_tlv_value_t readings[FLEET_TLV_MAX_READINGS];
    uint8_t reading_count;
} fleet_encode_input_t;

uint16_t fleet_tlv_crc16(const uint8_t *data, size_t len);

int fleet_tlv_encode(const fleet_encode_input_t *in, uint8_t *out, size_t out_len);

int fleet_tlv_decode(const uint8_t *in, size_t in_len, fleet_decoded_frame_t *out);

bool fleet_tlv_header_valid(const fleet_frame_header_t *hdr);

/** True when header carries cloud envelope fields from the host. */
bool fleet_tlv_header_has_envelope(const fleet_frame_header_t *hdr);

#ifdef __cplusplus
}
#endif
