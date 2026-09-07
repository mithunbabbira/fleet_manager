#include "uplink_queue.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

int uplink_queue_line_from_envelope(const char *envelope_json, uint64_t queued_at_ms, char *out,
                                    size_t out_len)
{
    if (!envelope_json || !out || out_len < 8) {
        return -1;
    }
    size_t n = strlen(envelope_json);
    if (n < 2 || envelope_json[n - 1] != '}') {
        return -1;
    }
    if (n + 40 > out_len) {
        return -1;
    }
    memcpy(out, envelope_json, n - 1);
    out[n - 1] = '\0';
    size_t off = n - 1;
    if (appendf(out, out_len, &off, ",\"queued_at_ms\":%llu}",
                (unsigned long long)queued_at_ms) != 0) {
        return -1;
    }
    return (int)off;
}

static int extract_queued_event(const char *line, size_t line_len, const char **obj,
                                size_t *obj_len)
{
    if (!line || !obj || !obj_len || line_len < 4 || line[0] != '{') {
        return -1;
    }
    const char *marker = ",\"queued_at_ms\":";
    size_t marker_len = strlen(marker);
    const char *end = line + line_len;
    const char *found = NULL;
    for (const char *s = line; s + marker_len <= end; s++) {
        if (memcmp(s, marker, marker_len) == 0) {
            found = s;
            break;
        }
    }
    if (!found) {
        return -1;
    }
    size_t obj_len_local = (size_t)(found - line);
    if (obj_len_local < 2) {
        return -1;
    }
    bool has_schema = false;
    const char *needle = "\"schemaId\":";
    size_t needle_len = strlen(needle);
    for (const char *s = line; s + needle_len <= line + obj_len_local; s++) {
        if (memcmp(s, needle, needle_len) == 0) {
            has_schema = true;
            break;
        }
    }
    if (!has_schema) {
        return -1;
    }
    *obj = line;
    *obj_len = obj_len_local;
    return 0;
}

int uplink_queue_build_batch(const char *events_blob, size_t n_events, char *out, size_t out_len)
{
    if (!events_blob || !out || out_len < 32 || n_events == 0) {
        return -1;
    }
    size_t off = 0;
    out[0] = '\0';
    if (append(out, out_len, &off, "[") != 0) {
        return -1;
    }

    const char *p = events_blob;
    size_t consumed = 0;
    size_t emitted = 0;
    while (*p && consumed < n_events) {
        while (*p == '\n' || *p == '\r') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *start = p;
        while (*p && *p != '\n' && *p != '\r') {
            p++;
        }
        size_t len = (size_t)(p - start);
        const char *obj = NULL;
        size_t obj_len = 0;
        if (extract_queued_event(start, len, &obj, &obj_len) == 0) {
            if (emitted > 0 && append(out, out_len, &off, ",") != 0) {
                break;
            }
            if (off + obj_len + 2 > out_len) {
                break;
            }
            memcpy(out + off, obj, obj_len);
            off += obj_len;
            out[off++] = '}';
            out[off] = '\0';
            emitted++;
        }
        consumed++;
        while (*p == '\n' || *p == '\r') {
            p++;
        }
    }
    if (emitted == 0) {
        return -1;
    }
    if (append(out, out_len, &off, "]") != 0) {
        return -1;
    }
    return (int)off;
}
