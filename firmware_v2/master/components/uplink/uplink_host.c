#include "uplink_host.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bool uplink_host_has_valid_reading(const uplink_host_report_t *host)
{
    if (host == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < host->reading_count && i < UPLINK_HOST_MAX_READINGS; i++) {
        if (host->readings[i].valid && host->readings[i].key[0] != '\0') {
            return true;
        }
    }
    return false;
}

static int append(char *out, size_t out_len, size_t *off, const char *s)
{
    size_t n = strlen(s);
    if (*off + n + 1 > out_len) {
        return -1;
    }
    memcpy(out + *off, s, n);
    *off += n;
    out[*off] = '\0';
    return 0;
}

static int appendf(char *out, size_t out_len, size_t *off, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *off, out_len > *off ? out_len - *off : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= (out_len > *off ? out_len - *off : 0)) {
        return -1;
    }
    *off += (size_t)n;
    return 0;
}

static int append_json_str(char *out, size_t out_len, size_t *off, const char *s)
{
    if (append(out, out_len, off, "\"") != 0) {
        return -1;
    }
    if (s == NULL) {
        s = "";
    }
    for (const char *p = s; *p; ++p) {
        char tmp[8];
        if (*p == '"' || *p == '\\') {
            snprintf(tmp, sizeof(tmp), "\\%c", *p);
            if (append(out, out_len, off, tmp) != 0) {
                return -1;
            }
        } else if ((unsigned char)*p < 0x20) {
            continue;
        } else {
            tmp[0] = *p;
            tmp[1] = '\0';
            if (append(out, out_len, off, tmp) != 0) {
                return -1;
            }
        }
    }
    return append(out, out_len, off, "\"");
}

int uplink_build_host_report_payload(const uplink_host_report_t *host, char *out, size_t out_len)
{
    if (!host || !out || out_len < 32 || host->device_id[0] == '\0') {
        return -1;
    }
    if (!uplink_host_has_valid_reading(host)) {
        return -1;
    }

    size_t off = 0;
    out[0] = '\0';
    if (append(out, out_len, &off, "{") != 0) {
        return -1;
    }
    if (append(out, out_len, &off, "\"host_type\":") != 0 ||
        append_json_str(out, out_len, &off, host->host_type) != 0) {
        return -1;
    }

    for (uint8_t ri = 0; ri < host->reading_count && ri < UPLINK_HOST_MAX_READINGS; ri++) {
        const uplink_host_reading_t *r = &host->readings[ri];
        if (!r->valid || r->key[0] == '\0') {
            continue;
        }
        /* Flat metric keys: "height_mm":130.7 — cloud schemas map by key name. */
        if (appendf(out, out_len, &off, ",\"%s\":%.4g", r->key, r->value) != 0) {
            return -1;
        }
        if (r->unit[0] != '\0') {
            char unit_key[40];
            snprintf(unit_key, sizeof(unit_key), "%s_unit", r->key);
            if (appendf(out, out_len, &off, ",\"%s\":", unit_key) != 0 ||
                append_json_str(out, out_len, &off, r->unit) != 0) {
                return -1;
            }
        }
    }

    if (append(out, out_len, &off, "}") != 0) {
        return -1;
    }
    return (int)off;
}
