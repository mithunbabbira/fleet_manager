#include "obd_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Extract contiguous hex bytes from ELM text into out[]; returns byte count */
static int extract_hex_bytes(const char *in, uint8_t *out, int max_out)
{
    if (!in || !out || max_out <= 0) return 0;
    int n = 0;
    int hi = -1;
    for (const char *p = in; *p && n < max_out; ++p) {
        if (*p == ' ' || *p == '\t' || *p == ':') continue;
        if (*p == '\r' || *p == '\n') {
            hi = -1;
            continue;
        }
        if (*p >= '0' && *p <= '9' && p[1] == ':') {
            ++p;
            hi = -1;
            continue;
        }
        int v = hex_nibble(*p);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else {
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    return n;
}

static void dtc_to_string(uint16_t raw, char out[6])
{
    const char *sys = "PCBU";
    out[0] = sys[(raw >> 14) & 0x3];
    out[1] = '0' + ((raw >> 12) & 0x3);
    static const char *hexd = "0123456789ABCDEF";
    out[2] = hexd[(raw >> 8) & 0xF];
    out[3] = hexd[(raw >> 4) & 0xF];
    out[4] = hexd[raw & 0xF];
    out[5] = '\0';
}

bool obd_codec_decode_mode01(const char *response, uint8_t pid, obd_decoded_t *out)
{
    if (!response || !out) return false;
    memset(out, 0, sizeof(*out));
    uint8_t bytes[64];
    int n = extract_hex_bytes(response, bytes, 64);
    int i = 0;
    while (i + 2 < n) {
        if (bytes[i] == 0x41 && bytes[i + 1] == pid) {
            uint8_t A = (i + 2 < n) ? bytes[i + 2] : 0;
            uint8_t B = (i + 3 < n) ? bytes[i + 3] : 0;
            snprintf(out->raw_hex, sizeof(out->raw_hex), "%02X%02X%02X%02X",
                     bytes[i], bytes[i + 1], A, B);
            out->ok = true;
            switch (pid) {
            case 0x0C:
                out->name = "rpm"; out->unit = "rpm";
                out->value = ((A * 256.0) + B) / 4.0; return true;
            case 0x0D:
                out->name = "speed"; out->unit = "km/h";
                out->value = A; return true;
            case 0x05:
                out->name = "coolant_c"; out->unit = "C";
                out->value = (double)A - 40.0; return true;
            case 0x11:
                out->name = "throttle_pct"; out->unit = "%";
                out->value = A * 100.0 / 255.0; return true;
            case 0x2F:
                out->name = "fuel_pct"; out->unit = "%";
                out->value = A * 100.0 / 255.0; return true;
            default:
                out->ok = false; return false;
            }
        }
        ++i;
    }
    return false;
}

int obd_codec_parse_dtcs(const char *response, char out[][6], int max_out)
{
    if (!response || !out || max_out <= 0) return 0;
    uint8_t bytes[64];
    int n = extract_hex_bytes(response, bytes, 64);
    int count = 0;
    for (int i = 0; i + 2 < n && count < max_out; ++i) {
        if (bytes[i] == 0x43 || bytes[i] == 0x47 || bytes[i] == 0x4A) {
            for (int j = i + 1; j + 1 < n && count < max_out; j += 2) {
                uint16_t raw = (uint16_t)((bytes[j] << 8) | bytes[j + 1]);
                if (raw == 0) continue;
                dtc_to_string(raw, out[count++]);
            }
            break;
        }
    }
    return count;
}

bool obd_codec_parse_vin(const char *response, char *vin, size_t vin_len)
{
    if (!vin || vin_len < 18) return false;
    uint8_t bytes[64];
    int n = extract_hex_bytes(response, bytes, 64);
    int start = -1;
    for (int i = 0; i + 2 < n; ++i) {
        if (bytes[i] == 0x49 && bytes[i + 1] == 0x02) { start = i + 2; break; }
    }
    if (start < 0) return false;
    /* first data byte after 49 02 is often item count; skip if non-ASCII */
    if (start < n && bytes[start] < 0x20) start++;
    size_t v = 0;
    for (int i = start; i < n && v < 17; ++i) {
        if (bytes[i] >= 0x20 && bytes[i] < 0x7F) vin[v++] = (char)bytes[i];
    }
    vin[v] = '\0';
    return v == 17;
}

bool obd_codec_decode_named(const char *decode_key, const char *response, obd_decoded_t *out)
{
    if (!decode_key || !out) return false;
    if (strcmp(decode_key, "rpm") == 0) return obd_codec_decode_mode01(response, 0x0C, out);
    if (strcmp(decode_key, "speed") == 0) return obd_codec_decode_mode01(response, 0x0D, out);
    if (strcmp(decode_key, "coolant_c") == 0) return obd_codec_decode_mode01(response, 0x05, out);
    if (strcmp(decode_key, "throttle_pct") == 0) return obd_codec_decode_mode01(response, 0x11, out);
    if (strcmp(decode_key, "fuel_pct") == 0) return obd_codec_decode_mode01(response, 0x2F, out);
    if (strcmp(decode_key, "voltage") == 0) {
        memset(out, 0, sizeof(*out));
        out->name = "voltage"; out->unit = "V";
        /* ATRV style: "12.6V" */
        out->value = atof(response);
        out->ok = out->value > 0.0;
        snprintf(out->raw_hex, sizeof(out->raw_hex), "%s", response);
        return out->ok;
    }
    return false;
}
