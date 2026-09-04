#include "fleet_tlv.h"

#include <string.h>

static size_t tlv_write(uint8_t *out, size_t out_len, size_t off, uint16_t id,
                        fleet_value_type_t type, const void *val, size_t val_len)
{
    if (off + 3 + val_len > out_len) {
        return 0;
    }
    out[off++] = (uint8_t)(id & 0xFF);
    out[off++] = (uint8_t)type;
    out[off++] = (uint8_t)val_len;
    memcpy(out + off, val, val_len);
    return off + val_len;
}

static size_t append_string_tlv(uint8_t *buf, size_t buf_len, size_t off, uint16_t id,
                                const char *s, size_t max_len)
{
    if (!s || !s[0]) {
        return off;
    }
    size_t n = strlen(s);
    if (n >= max_len) {
        n = max_len - 1;
    }
    size_t next = tlv_write(buf, buf_len, off, id, FLEET_VAL_STRING, s, n);
    return next ? next : 0;
}

uint16_t fleet_tlv_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static int append_header_tlvs(const fleet_frame_header_t *hdr, uint8_t *buf, size_t buf_len,
                              size_t *off)
{
    size_t o = *off;
    if (!hdr->device_id[0]) {
        return -1;
    }
    size_t id_len = strlen(hdr->device_id);
    if (id_len >= FLEET_DEVICE_ID_MAX) {
        id_len = FLEET_DEVICE_ID_MAX - 1;
    }
    o = tlv_write(buf, buf_len, o, FLEET_TLV_DEVICE_ID, FLEET_VAL_STRING, hdr->device_id, id_len);
    if (!o) {
        return -1;
    }

    uint16_t ht = hdr->host_type_id;
    o = tlv_write(buf, buf_len, o, FLEET_TLV_HOST_TYPE_ID, FLEET_VAL_UINT16, &ht, sizeof(ht));
    if (!o) {
        return -1;
    }

    o = tlv_write(buf, buf_len, o, FLEET_TLV_SEQ, FLEET_VAL_UINT8, &hdr->seq, 1);
    if (!o) {
        return -1;
    }

    o = tlv_write(buf, buf_len, o, FLEET_TLV_TS_MS, FLEET_VAL_INT32, &hdr->ts_ms, sizeof(int32_t));
    if (!o) {
        return -1;
    }
    uint32_t ts_hi = (uint32_t)(hdr->ts_ms >> 32);
    o = tlv_write(buf, buf_len, o, FLEET_TLV_TS_MS_HI, FLEET_VAL_UINT16, &ts_hi, sizeof(uint16_t));
    if (!o) {
        return -1;
    }

    o = tlv_write(buf, buf_len, o, FLEET_TLV_STATUS, FLEET_VAL_UINT8, &hdr->status, 1);
    if (!o) {
        return -1;
    }

    if (hdr->manifest_version) {
        o = tlv_write(buf, buf_len, o, FLEET_TLV_MANIFEST_VER, FLEET_VAL_UINT8,
                      &hdr->manifest_version, 1);
        if (!o) {
            return -1;
        }
    }

    o = append_string_tlv(buf, buf_len, o, FLEET_TLV_NODE_ID, hdr->node_id, FLEET_NODE_ID_MAX);
    if (!o) {
        return -1;
    }
    o = append_string_tlv(buf, buf_len, o, FLEET_TLV_SCHEMA_ID, hdr->schema_id, FLEET_SCHEMA_ID_MAX);
    if (!o) {
        return -1;
    }
    o = append_string_tlv(buf, buf_len, o, FLEET_TLV_HOST_TYPE, hdr->host_type,
                          FLEET_HOST_TYPE_NAME_MAX);
    if (!o) {
        return -1;
    }

    *off = o;
    return 0;
}

int fleet_tlv_encode(const fleet_encode_input_t *in, uint8_t *out, size_t out_len)
{
    if (!in || !out || out_len < 8) {
        return -1;
    }

    uint8_t hdr_buf[128];
    size_t hdr_off = 0;
    if (append_header_tlvs(&in->header, hdr_buf, sizeof(hdr_buf), &hdr_off) != 0) {
        return -1;
    }

    uint8_t body_buf[FLEET_TLV_MAX_FRAME];
    size_t body_off = 0;
    for (uint8_t i = 0; i < in->reading_count; i++) {
        const fleet_tlv_value_t *r = &in->readings[i];
        size_t n = 0;
        switch (r->type) {
        case FLEET_VAL_FLOAT:
            n = tlv_write(body_buf, sizeof(body_buf), body_off, r->tlv_id, FLEET_VAL_FLOAT,
                          &r->value.f32, sizeof(float));
            break;
        case FLEET_VAL_UINT8:
            n = tlv_write(body_buf, sizeof(body_buf), body_off, r->tlv_id, FLEET_VAL_UINT8,
                          &r->value.u8, 1);
            break;
        case FLEET_VAL_UINT16:
            n = tlv_write(body_buf, sizeof(body_buf), body_off, r->tlv_id, FLEET_VAL_UINT16,
                          &r->value.u16, sizeof(uint16_t));
            break;
        case FLEET_VAL_INT32:
            n = tlv_write(body_buf, sizeof(body_buf), body_off, r->tlv_id, FLEET_VAL_INT32,
                          &r->value.i32, sizeof(int32_t));
            break;
        case FLEET_VAL_STRING: {
            size_t slen = strlen(r->value.str);
            if (slen >= sizeof(r->value.str)) {
                slen = sizeof(r->value.str) - 1;
            }
            n = tlv_write(body_buf, sizeof(body_buf), body_off, r->tlv_id, FLEET_VAL_STRING,
                          r->value.str, slen);
            break;
        }
        default:
            return -1;
        }
        if (!n) {
            return -1;
        }
        body_off = n;
    }

    size_t total = 4 + hdr_off + body_off + 2;
    if (total > out_len || total > FLEET_TLV_MAX_FRAME) {
        return -1;
    }

    out[0] = FLEET_TLV_MAGIC;
    out[1] = FLEET_TLV_VERSION;
    out[2] = (uint8_t)in->msg_type;
    out[3] = (uint8_t)hdr_off;
    memcpy(out + 4, hdr_buf, hdr_off);
    memcpy(out + 4 + hdr_off, body_buf, body_off);

    uint16_t crc = fleet_tlv_crc16(out, 4 + hdr_off + body_off);
    out[4 + hdr_off + body_off] = (uint8_t)(crc & 0xFF);
    out[4 + hdr_off + body_off + 1] = (uint8_t)((crc >> 8) & 0xFF);
    return (int)total;
}

static void copy_header_string(char *dst, size_t dst_len, const uint8_t *src, uint8_t vlen)
{
    if (vlen >= dst_len) {
        return;
    }
    memcpy(dst, src, vlen);
    dst[vlen] = '\0';
}

static int parse_tlv_block(const uint8_t *buf, size_t len, fleet_decoded_frame_t *out,
                           bool is_header)
{
    size_t off = 0;
    while (off + 3 <= len) {
        uint16_t id = buf[off++];
        fleet_value_type_t type = (fleet_value_type_t)buf[off++];
        uint8_t vlen = buf[off++];
        if (off + vlen > len) {
            return -1;
        }

        if (is_header) {
            switch (id) {
            case FLEET_TLV_DEVICE_ID:
                if (type == FLEET_VAL_STRING) {
                    copy_header_string(out->header.device_id, sizeof(out->header.device_id),
                                       buf + off, vlen);
                }
                break;
            case FLEET_TLV_NODE_ID:
                if (type == FLEET_VAL_STRING) {
                    copy_header_string(out->header.node_id, sizeof(out->header.node_id), buf + off,
                                       vlen);
                }
                break;
            case FLEET_TLV_SCHEMA_ID:
                if (type == FLEET_VAL_STRING) {
                    copy_header_string(out->header.schema_id, sizeof(out->header.schema_id),
                                       buf + off, vlen);
                }
                break;
            case FLEET_TLV_HOST_TYPE:
                if (type == FLEET_VAL_STRING) {
                    copy_header_string(out->header.host_type, sizeof(out->header.host_type),
                                       buf + off, vlen);
                }
                break;
            case FLEET_TLV_HOST_TYPE_ID:
                if (type == FLEET_VAL_UINT16 && vlen >= 2) {
                    out->header.host_type_id =
                        (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
                }
                break;
            case FLEET_TLV_SEQ:
                if (type == FLEET_VAL_UINT8 && vlen >= 1) {
                    out->header.seq = buf[off];
                }
                break;
            case FLEET_TLV_TS_MS:
                if (type == FLEET_VAL_INT32 && vlen >= 4) {
                    int32_t lo = (int32_t)(buf[off] | ((uint32_t)buf[off + 1] << 8) |
                                           ((uint32_t)buf[off + 2] << 16) |
                                           ((uint32_t)buf[off + 3] << 24));
                    out->header.ts_ms = (uint64_t)(uint32_t)lo;
                }
                break;
            case FLEET_TLV_TS_MS_HI:
                if (type == FLEET_VAL_UINT16 && vlen >= 2) {
                    uint16_t hi = (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
                    out->header.ts_ms |= ((uint64_t)hi) << 32;
                }
                break;
            case FLEET_TLV_STATUS:
                if (type == FLEET_VAL_UINT8 && vlen >= 1) {
                    out->header.status = buf[off];
                }
                break;
            case FLEET_TLV_MANIFEST_VER:
                if (type == FLEET_VAL_UINT8 && vlen >= 1) {
                    out->header.manifest_version = buf[off];
                }
                break;
            default:
                break;
            }
        } else if (out->reading_count < FLEET_TLV_MAX_READINGS) {
            fleet_tlv_value_t *r = &out->readings[out->reading_count++];
            r->tlv_id = id;
            r->type = type;
            r->valid = true;
            switch (type) {
            case FLEET_VAL_FLOAT:
                if (vlen >= 4) {
                    memcpy(&r->value.f32, buf + off, 4);
                } else {
                    r->valid = false;
                }
                break;
            case FLEET_VAL_UINT8:
                r->value.u8 = vlen >= 1 ? buf[off] : 0;
                break;
            case FLEET_VAL_UINT16:
                if (vlen >= 2) {
                    r->value.u16 = (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
                } else {
                    r->valid = false;
                }
                break;
            case FLEET_VAL_INT32:
                if (vlen >= 4) {
                    memcpy(&r->value.i32, buf + off, 4);
                } else {
                    r->valid = false;
                }
                break;
            case FLEET_VAL_STRING:
                if (vlen < sizeof(r->value.str)) {
                    memcpy(r->value.str, buf + off, vlen);
                    r->value.str[vlen] = '\0';
                } else {
                    r->valid = false;
                }
                break;
            default:
                r->valid = false;
                break;
            }
        }
        off += vlen;
    }
    return 0;
}

int fleet_tlv_decode(const uint8_t *in, size_t in_len, fleet_decoded_frame_t *out)
{
    if (!in || !out || in_len < 6) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    if (in[0] != FLEET_TLV_MAGIC || in[1] != FLEET_TLV_VERSION) {
        return -1;
    }

    out->msg_type = (fleet_msg_type_t)in[2];
    uint8_t hdr_len = in[3];
    if ((size_t)4 + hdr_len + 2 > in_len) {
        return -1;
    }

    size_t body_len = in_len - 4 - hdr_len - 2;
    uint16_t crc = fleet_tlv_crc16(in, in_len - 2);
    uint16_t got = (uint16_t)(in[in_len - 2] | ((uint16_t)in[in_len - 1] << 8));
    if (crc != got) {
        return -1;
    }

    if (parse_tlv_block(in + 4, hdr_len, out, true) != 0) {
        return -1;
    }
    if (body_len > 0 && parse_tlv_block(in + 4 + hdr_len, body_len, out, false) != 0) {
        return -1;
    }
    return 0;
}

bool fleet_tlv_header_has_envelope(const fleet_frame_header_t *hdr)
{
    return hdr != NULL && hdr->node_id[0] != '\0' && hdr->schema_id[0] != '\0';
}

bool fleet_tlv_header_valid(const fleet_frame_header_t *hdr)
{
    if (!hdr || !hdr->device_id[0]) {
        return false;
    }
    /* Legacy: known host_type_id. Dynamic: cloud envelope from host. */
    if (hdr->host_type_id != 0) {
        return true;
    }
    return fleet_tlv_header_has_envelope(hdr);
}
