#include "uplink_envelope.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void uplink_virtual_gps_ids(const char *carrier_device_id, char *gps_device_id, size_t gps_dev_len,
                            char *gps_node_id, size_t gps_node_len)
{
    /* Derived only from provisioned master device_id (NVS/USB) — never from OTA. */
    const char *master =
        (carrier_device_id != NULL && carrier_device_id[0] != '\0') ? carrier_device_id : "unknown";
    if (gps_device_id && gps_dev_len) {
        snprintf(gps_device_id, gps_dev_len, "%s_GPS", master);
    }
    if (gps_node_id && gps_node_len) {
        snprintf(gps_node_id, gps_node_len, "node-%s_GPS", master);
    }
}

double uplink_gps_distance_m(double lat1, double lng1, double lat2, double lng2)
{
    const double r_m = 6371000.0;
    const double deg = 3.14159265358979323846 / 180.0;
    double p1 = lat1 * deg;
    double p2 = lat2 * deg;
    double dp = (lat2 - lat1) * deg;
    double dl = (lng2 - lng1) * deg;
    double a = sin(dp / 2.0) * sin(dp / 2.0) +
               cos(p1) * cos(p2) * sin(dl / 2.0) * sin(dl / 2.0);
    if (a < 0.0) {
        a = 0.0;
    }
    if (a > 1.0) {
        a = 1.0;
    }
    return 2.0 * r_m * atan2(sqrt(a), sqrt(1.0 - a));
}

bool uplink_gps_worth_sending(bool have_last, double last_lat, double last_lng, uint64_t last_ms,
                              double lat, double lng, uint64_t now_ms)
{
    if (!have_last) {
        return true;
    }
    if (uplink_gps_distance_m(last_lat, last_lng, lat, lng) >= UPLINK_GPS_ONLY_MIN_MOVE_M) {
        return true;
    }
    if (now_ms >= last_ms && (now_ms - last_ms) >= UPLINK_GPS_ONLY_HEARTBEAT_MS) {
        return true;
    }
    return false;
}

int uplink_build_gps_payload(bool gps_ok, double lat, double lng, char *out, size_t out_len)
{
    if (out == NULL || out_len < 32) {
        return -1;
    }
    int n;
    if (gps_ok) {
        n = snprintf(out, out_len, "{\"gps_ok\":true,\"lat\":%.7f,\"lng\":%.7f}", lat, lng);
    } else {
        n = snprintf(out, out_len, "{\"gps_ok\":false}");
    }
    return (n < 0 || (size_t)n >= out_len) ? -1 : n;
}

static int json_escape_str(char *out, size_t out_len, size_t *off, const char *s)
{
    if (s == NULL) {
        s = "";
    }
    if (*off + 1 >= out_len) {
        return -1;
    }
    out[(*off)++] = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        char esc[8];
        size_t el = 0;
        if (*p == '"' || *p == '\\') {
            esc[0] = '\\';
            esc[1] = (char)*p;
            el = 2;
        } else if (*p < 0x20) {
            el = (size_t)snprintf(esc, sizeof(esc), "\\u%04x", *p);
        } else {
            esc[0] = (char)*p;
            el = 1;
        }
        if (*off + el + 1 >= out_len) {
            return -1;
        }
        memcpy(out + *off, esc, el);
        *off += el;
    }
    if (*off + 1 >= out_len) {
        return -1;
    }
    out[(*off)++] = '"';
    out[*off] = '\0';
    return 0;
}

int uplink_build_envelope(const char *device_id, const char *node_id, const char *schema_id,
                          uint64_t ts_ms, const char *payload_json, char *out, size_t out_len)
{
    if (device_id == NULL || device_id[0] == '\0' || node_id == NULL || node_id[0] == '\0' ||
        schema_id == NULL || schema_id[0] == '\0' || payload_json == NULL || out == NULL ||
        out_len < 64) {
        return -1;
    }
    size_t off = 0;
    out[0] = '\0';

#define PUT(str)                                                                                   \
    do {                                                                                           \
        size_t _n = strlen(str);                                                                   \
        if (off + _n + 1 > out_len)                                                                \
            return -1;                                                                             \
        memcpy(out + off, str, _n);                                                                \
        off += _n;                                                                                 \
        out[off] = '\0';                                                                           \
    } while (0)

    PUT("{");
    PUT("\"device_id\":");
    if (json_escape_str(out, out_len, &off, device_id) != 0) {
        return -1;
    }
    PUT(",\"node_id\":");
    if (json_escape_str(out, out_len, &off, node_id) != 0) {
        return -1;
    }
    PUT(",\"schemaId\":");
    if (json_escape_str(out, out_len, &off, schema_id) != 0) {
        return -1;
    }
    char tsbuf[48];
    snprintf(tsbuf, sizeof(tsbuf), ",\"ts_ms\":%llu,\"payload\":", (unsigned long long)ts_ms);
    PUT(tsbuf);
    size_t plen = strlen(payload_json);
    if (off + plen + 2 > out_len) {
        return -1;
    }
    memcpy(out + off, payload_json, plen);
    off += plen;
    out[off++] = '}';
    out[off] = '\0';
#undef PUT
    return (int)off;
}
