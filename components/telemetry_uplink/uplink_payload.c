#include "uplink_payload.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * Builds Trafyn event JSON. Host-testable (no ESP-IDF).
 *
 * Live POST body (one event):
 *   {"schemaId":"1087","payload":{...}}
 * Batch POST body (drain):
 *   [{"schemaId":"1087","payload":{...}}, ...]
 * SD queue line (not sent as-is):
 *   {"payload":{...},"queued_at_ms":N}  — schemaId added only when building the batch.
 */

bool uplink_pid_is_fresh_ok(const uplink_pid_view_t *p)
{
    if (p == NULL) {
        return false;
    }
    return p->valid && p->ok && p->age_ms <= UPLINK_PID_FRESH_MS;
}

/** @brief Enqueue if fresh OBD PID or live GPS. */
bool uplink_should_enqueue(bool have_fresh_pid, bool gps_ok)
{
    return have_fresh_pid || gps_ok;
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

/** @brief GPS-only: first fix, ≥50 m move, or 5 min heartbeat. */
bool uplink_gps_only_worth_sending(bool have_last, double last_lat, double last_lng,
                                   uint64_t last_ms, double lat, double lng, uint64_t now_ms)
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

static int append(char *out, size_t out_len, size_t *off, const char *chunk)
{
    if (!out || !off || !chunk) {
        return -1;
    }
    size_t n = strlen(chunk);
    if (*off + n + 1 > out_len) {
        return -1;
    }
    memcpy(out + *off, chunk, n);
    *off += n;
    out[*off] = '\0';
    return 0;
}

static int appendf(char *out, size_t out_len, size_t *off, const char *fmt, ...)
{
    if (!out || !off || !fmt) {
        return -1;
    }
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

/** @brief JSON string escape (quotes/backslashes). */
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

/** @brief Emit PID fields when fresh-ok; always emit <ok> bool (never null). */
static int append_pid_fields(char *out, size_t out_len, size_t *off,
                             const char *num_key, const char *raw_key,
                             const char *age_key, const char *ok_key,
                             const uplink_pid_view_t *p, bool raw_is_string)
{
    (void)raw_is_string;
    if (uplink_pid_is_fresh_ok(p)) {
        if (appendf(out, out_len, off, ",\"%s\":%.4g", num_key, p->value) != 0) {
            return -1;
        }
        if (appendf(out, out_len, off, ",\"%s\":", raw_key) != 0 ||
            append_json_str(out, out_len, off, p->raw) != 0) {
            return -1;
        }
        if (appendf(out, out_len, off, ",\"%s\":%u", age_key,
                    (unsigned)p->age_ms) != 0) {
            return -1;
        }
    }

    if (appendf(out, out_len, off, ",\"%s\":%s", ok_key,
                uplink_pid_is_fresh_ok(p) ? "true" : "false") != 0) {
        return -1;
    }
    return 0;
}

/** @brief Inner payload object only (PIDs, gps, device/node ids). */
int uplink_payload_build_payload(const uplink_snapshot_t *snap, char *out, size_t out_len)
{
    if (!snap || !out || out_len < 32) {
        return -1;
    }
    size_t off = 0;
    out[0] = '\0';

    if (append(out, out_len, &off, "{") != 0) {
        return -1;
    }
    if (append(out, out_len, &off, "\"device_id\":") != 0 ||
        append_json_str(out, out_len, &off, snap->device_id) != 0) {
        return -1;
    }
    if (append(out, out_len, &off, ",\"node_id\":") != 0 ||
        append_json_str(out, out_len, &off, snap->node_id) != 0) {
        return -1;
    }
    if (appendf(out, out_len, &off, ",\"schema_version\":1") != 0) {
        return -1;
    }
    if (appendf(out, out_len, &off, ",\"ts_ms\":%llu",
                (unsigned long long)snap->ts_ms) != 0) {
        return -1;
    }
    if (append(out, out_len, &off, ",\"obd_profile\":") != 0 ||
        append_json_str(out, out_len, &off, snap->obd_profile) != 0) {
        return -1;
    }
    if (append(out, out_len, &off, ",\"obd_protocol\":") != 0 ||
        append_json_str(out, out_len, &off, snap->obd_protocol) != 0) {
        return -1;
    }
    if (appendf(out, out_len, &off, ",\"uptime_seconds\":%u,\"poller_status\":",
                (unsigned)snap->uptime_seconds) != 0) {
        return -1;
    }
    if (append_json_str(out, out_len, &off,
                        snap->poller_status ? snap->poller_status : "paused") != 0) {
        return -1;
    }
    if (appendf(out, out_len, &off,
                ",\"cmds_ok\":%llu,\"cmds_fail\":%llu,"
                "\"blocked_cmds\":%llu,\"telemetry_drops\":%llu",
                (unsigned long long)snap->cmds_ok,
                (unsigned long long)snap->cmds_fail,
                (unsigned long long)snap->blocked_cmds,
                (unsigned long long)snap->telemetry_drops) != 0) {
        return -1;
    }

    if (append_pid_fields(out, out_len, &off, "rpm", "rpm_raw_hex", "rpm_age_ms", "rpm_ok",
                          &snap->rpm, true) != 0) {
        return -1;
    }
    if (append_pid_fields(out, out_len, &off, "speed_kmh", "speed_raw_hex", "speed_age_ms",
                          "speed_ok", &snap->speed, true) != 0) {
        return -1;
    }
    if (append_pid_fields(out, out_len, &off, "coolant_c", "coolant_raw_hex", "coolant_age_ms",
                          "coolant_ok", &snap->coolant, true) != 0) {
        return -1;
    }
    if (append_pid_fields(out, out_len, &off, "throttle_pct", "throttle_raw_hex",
                          "throttle_age_ms", "throttle_ok", &snap->throttle, true) != 0) {
        return -1;
    }
    if (append_pid_fields(out, out_len, &off, "voltage_v", "voltage_raw", "voltage_age_ms",
                          "voltage_ok", &snap->voltage, true) != 0) {
        return -1;
    }

    if (appendf(out, out_len, &off, ",\"gps_ok\":%s",
                snap->gps_ok ? "true" : "false") != 0) {
        return -1;
    }
    if (snap->gps_ok) {
        if (appendf(out, out_len, &off, ",\"lat\":%.7f,\"lng\":%.7f",
                    snap->lat, snap->lng) != 0) {
            return -1;
        }
    }

    if (snap->host_count > 0) {
        if (append(out, out_len, &off, ",\"hosts\":[") != 0) {
            return -1;
        }
        for (uint8_t hi = 0; hi < snap->host_count && hi < UPLINK_MAX_HOSTS; hi++) {
            const uplink_host_report_t *h = &snap->hosts[hi];
            if (hi > 0 && append(out, out_len, &off, ",") != 0) {
                return -1;
            }
            if (appendf(out, out_len, &off, "{\"device_id\":") != 0 ||
                append_json_str(out, out_len, &off, h->device_id) != 0 ||
                append(out, out_len, &off, ",\"host_type\":") != 0 ||
                append_json_str(out, out_len, &off, h->host_type) != 0 ||
                appendf(out, out_len, &off, ",\"host_type_id\":%u,\"readings\":[",
                            (unsigned)h->host_type_id) != 0) {
                return -1;
            }
            for (uint8_t ri = 0; ri < h->reading_count && ri < UPLINK_MAX_HOST_READINGS; ri++) {
                const uplink_host_reading_t *r = &h->readings[ri];
                if (ri > 0 && append(out, out_len, &off, ",") != 0) {
                    return -1;
                }
                if (appendf(out, out_len, &off, "{\"key\":") != 0 ||
                    append_json_str(out, out_len, &off, r->key) != 0 ||
                    appendf(out, out_len, &off, ",\"value\":%.4g,\"unit\":") != 0 ||
                    append_json_str(out, out_len, &off, r->unit) != 0 ||
                    appendf(out, out_len, &off, ",\"valid\":%s}",
                                r->valid ? "true" : "false") != 0) {
                    return -1;
                }
            }
            if (appendf(out, out_len, &off, "],\"ts_ms\":%llu}",
                        (unsigned long long)h->ts_ms) != 0) {
                return -1;
            }
        }
        if (append(out, out_len, &off, "]") != 0) {
            return -1;
        }
    }

    if (append(out, out_len, &off, ",\"source\":\"esp32_obd\"}") != 0) {
        return -1;
    }
    return (int)off;
}

static const char *schema_or_default(const char *schema_id)
{
    if (schema_id != NULL && schema_id[0] != '\0') {
        return schema_id;
    }
    return UPLINK_SCHEMA_ID;
}

/** @brief Live body {"schemaId","payload"}; length or -1. */
int uplink_payload_build(const uplink_snapshot_t *snap, const char *schema_id, char *out,
                         size_t out_len)
{
    /* Live / no-SD path: wrap payload with schemaId for a single-object POST body. */
    if (!snap || !out || out_len < 32) {
        return -1;
    }
    char payload[1800];
    int pn = uplink_payload_build_payload(snap, payload, sizeof(payload));
    if (pn < 0) {
        return -1;
    }
    size_t off = 0;
    out[0] = '\0';
    if (append(out, out_len, &off, "{\"schemaId\":\"") != 0) {
        return -1;
    }
    if (append(out, out_len, &off, schema_or_default(schema_id)) != 0) {
        return -1;
    }
    if (append(out, out_len, &off, "\",\"payload\":") != 0) {
        return -1;
    }
    if (append(out, out_len, &off, payload) != 0) {
        return -1;
    }
    if (append(out, out_len, &off, "}") != 0) {
        return -1;
    }
    return (int)off;
}

/** @brief NDJSON queue line with queued_at_ms. */
int uplink_payload_build_queued_event(const char *payload_json, uint64_t queued_at_ms,
                                      char *out, size_t out_len)
{
    if (!payload_json || !out || out_len < 32) {
        return -1;
    }
    size_t off = 0;
    out[0] = '\0';
    if (append(out, out_len, &off, "{\"payload\":") != 0) {
        return -1;
    }
    if (append(out, out_len, &off, payload_json) != 0) {
        return -1;
    }
    if (appendf(out, out_len, &off, ",\"queued_at_ms\":%llu}",
                (unsigned long long)queued_at_ms) != 0) {
        return -1;
    }
    return (int)off;
}

/* Extract the JSON object that follows "payload": in a queued event line. */
/** @brief Brace-match object after "payload": in a queue line. */
static int extract_payload_object(const char *line, size_t line_len, const char **obj,
                                  size_t *obj_len)
{
    if (!line || !obj || !obj_len || line_len < 12) {
        return -1;
    }
    const char *key = "\"payload\":";
    size_t key_len = 10;
    const char *end = line + line_len;
    const char *found = NULL;
    for (const char *s = line; s + key_len <= end; s++) {
        if (memcmp(s, key, key_len) == 0) {
            found = s + key_len;
            break;
        }
    }
    if (!found) {
        return -1;
    }
    while (found < end && (*found == ' ' || *found == '\t')) {
        found++;
    }
    if (found >= end || *found != '{') {
        return -1;
    }
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    const char *q = found;
    for (; q < end; q++) {
        char c = *q;
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                *obj = found;
                *obj_len = (size_t)(q - found + 1);
                return 0;
            }
        }
    }
    return -1;
}

/** @brief Batch array re-wrapping each queued payload with schemaId. */
int uplink_payload_build_batch(const char *events_blob, size_t n_events, const char *schema_id,
                               char *out, size_t out_len)
{
    /*
     * Drain path: turn SD NDJSON lines into the Trafyn array POST body.
     * Each line's payload object is re-wrapped with UPLINK_SCHEMA_ID; queued_at_ms
     * is intentionally omitted from the HTTP body (local queue metadata only).
     */
    if (!events_blob || !out || out_len < 32 || n_events == 0) {
        return -1;
    }
    size_t off = 0;
    out[0] = '\0';
    if (append(out, out_len, &off, "[") != 0) {
        return -1;
    }

    /* events_blob is newline-separated queued objects. */
    const char *p = events_blob;
    size_t emitted = 0;
    while (*p && emitted < n_events) {
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
        if (len == 0) {
            continue;
        }
        const char *payload_obj = NULL;
        size_t payload_len = 0;
        if (extract_payload_object(start, len, &payload_obj, &payload_len) != 0) {
            return -1;
        }
        if (emitted > 0) {
            if (append(out, out_len, &off, ",") != 0) {
                return -1;
            }
        }
        if (append(out, out_len, &off, "{\"schemaId\":\"") != 0) {
            return -1;
        }
        if (append(out, out_len, &off, schema_or_default(schema_id)) != 0) {
            return -1;
        }
        if (append(out, out_len, &off, "\",\"payload\":") != 0) {
            return -1;
        }
        if (off + payload_len + 2 > out_len) {
            return -1;
        }
        memcpy(out + off, payload_obj, payload_len);
        off += payload_len;
        out[off] = '\0';
        if (append(out, out_len, &off, "}") != 0) {
            return -1;
        }
        emitted++;
    }
    if (emitted == 0) {
        return -1;
    }
    if (append(out, out_len, &off, "]") != 0) {
        return -1;
    }
    return (int)off;
}
