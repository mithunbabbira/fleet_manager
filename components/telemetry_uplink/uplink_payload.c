#include "uplink_payload.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * Builds Trafyn event JSON. Host-testable (no ESP-IDF).
 *
 * Envelope per event:
 *   {"device_id","node_id","schemaId","ts_ms","payload":{...}}
 *
 * Live POST: single object or array of envelopes.
 * SD queue line: envelope + ,"queued_at_ms":N
 * Batch POST: JSON array of envelopes (queued_at_ms stripped).
 */

bool uplink_pid_is_fresh_ok(const uplink_pid_view_t *p)
{
    if (p == NULL) {
        return false;
    }
    return p->valid && p->ok && p->age_ms <= UPLINK_PID_FRESH_MS;
}

bool uplink_snapshot_has_fresh_pid(const uplink_snapshot_t *snap)
{
    if (snap == NULL) {
        return false;
    }
    return uplink_pid_is_fresh_ok(&snap->rpm) || uplink_pid_is_fresh_ok(&snap->speed) ||
           uplink_pid_is_fresh_ok(&snap->coolant) || uplink_pid_is_fresh_ok(&snap->throttle) ||
           uplink_pid_is_fresh_ok(&snap->voltage);
}

bool uplink_snapshot_has_valid_host_reading(const uplink_snapshot_t *snap)
{
    if (snap == NULL) {
        return false;
    }
    for (uint8_t hi = 0; hi < snap->host_count && hi < UPLINK_MAX_HOSTS; hi++) {
        const uplink_host_report_t *h = &snap->hosts[hi];
        for (uint8_t ri = 0; ri < h->reading_count && ri < UPLINK_MAX_HOST_READINGS; ri++) {
            if (h->readings[ri].valid) {
                return true;
            }
        }
    }
    return false;
}

bool uplink_should_enqueue(bool have_fresh_pid, bool gps_ok)
{
    return have_fresh_pid || gps_ok;
}

bool uplink_tick_worth_producing(bool have_fresh_pid, bool gps_ok, bool gps_worth,
                                 const uplink_snapshot_t *snap)
{
    if (have_fresh_pid) {
        return true;
    }
    if (gps_ok && gps_worth) {
        return true;
    }
    return uplink_snapshot_has_valid_host_reading(snap);
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

static void copy_id_suffix(const char *id, char *suffix, size_t suffix_len)
{
    if (!id || !suffix || suffix_len == 0) {
        return;
    }
    const char *dash = strrchr(id, '-');
    if (dash != NULL && dash[1] != '\0') {
        snprintf(suffix, suffix_len, "%s", dash + 1);
    } else {
        snprintf(suffix, suffix_len, "%s", id ? id : "");
    }
}

void uplink_virtual_gps_ids(const char *carrier_device_id, const char *carrier_node_id,
                            char *gps_device_id, size_t gps_dev_len, char *gps_node_id,
                            size_t gps_node_len)
{
    char suffix[40];
    copy_id_suffix(carrier_device_id, suffix, sizeof(suffix));
    snprintf(gps_device_id, gps_dev_len, "gps-%s", suffix);
    (void)carrier_node_id;
    snprintf(gps_node_id, gps_node_len, "node-gps-%s", suffix);
}

void uplink_host_node_id(const char *host_device_id, char *node_id, size_t node_len)
{
    snprintf(node_id, node_len, "node-%s", host_device_id ? host_device_id : "");
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

static int append_pid_fields(char *out, size_t out_len, size_t *off, const char *num_key,
                             const char *raw_key, const char *age_key, const char *ok_key,
                             const uplink_pid_view_t *p)
{
    if (uplink_pid_is_fresh_ok(p)) {
        if (appendf(out, out_len, off, ",\"%s\":%.4g", num_key, p->value) != 0) {
            return -1;
        }
        if (appendf(out, out_len, off, ",\"%s\":", raw_key) != 0 ||
            append_json_str(out, out_len, off, p->raw) != 0) {
            return -1;
        }
        if (appendf(out, out_len, off, ",\"%s\":%u", age_key, (unsigned)p->age_ms) != 0) {
            return -1;
        }
    }
    if (appendf(out, out_len, off, ",\"%s\":%s", ok_key,
                uplink_pid_is_fresh_ok(p) ? "true" : "false") != 0) {
        return -1;
    }
    return 0;
}

int uplink_payload_build_obd_payload(const uplink_snapshot_t *snap, char *out, size_t out_len)
{
    if (!snap || !out || out_len < 32) {
        return -1;
    }
    size_t off = 0;
    out[0] = '\0';
    if (append(out, out_len, &off, "{") != 0) {
        return -1;
    }
    if (append(out, out_len, &off, "\"obd_profile\":") != 0 ||
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
                (unsigned long long)snap->cmds_ok, (unsigned long long)snap->cmds_fail,
                (unsigned long long)snap->blocked_cmds,
                (unsigned long long)snap->telemetry_drops) != 0) {
        return -1;
    }
    if (append_pid_fields(out, out_len, &off, "rpm", "rpm_raw_hex", "rpm_age_ms", "rpm_ok",
                          &snap->rpm) != 0) {
        return -1;
    }
    if (append_pid_fields(out, out_len, &off, "speed_kmh", "speed_raw_hex", "speed_age_ms",
                          "speed_ok", &snap->speed) != 0) {
        return -1;
    }
    if (append_pid_fields(out, out_len, &off, "coolant_c", "coolant_raw_hex", "coolant_age_ms",
                          "coolant_ok", &snap->coolant) != 0) {
        return -1;
    }
    if (append_pid_fields(out, out_len, &off, "throttle_pct", "throttle_raw_hex",
                          "throttle_age_ms", "throttle_ok", &snap->throttle) != 0) {
        return -1;
    }
    if (append_pid_fields(out, out_len, &off, "voltage_v", "voltage_raw", "voltage_age_ms",
                          "voltage_ok", &snap->voltage) != 0) {
        return -1;
    }
    if (append(out, out_len, &off, ",\"source\":\"esp32_obd\"}") != 0) {
        return -1;
    }
    return (int)off;
}

int uplink_payload_build_gps_payload(const uplink_snapshot_t *snap, char *out, size_t out_len)
{
    if (!snap || !out || out_len < 32) {
        return -1;
    }
    size_t off = 0;
    out[0] = '\0';
    if (append(out, out_len, &off, "{") != 0) {
        return -1;
    }
    if (appendf(out, out_len, &off, "\"gps_ok\":%s", snap->gps_ok ? "true" : "false") != 0) {
        return -1;
    }
    if (snap->gps_ok) {
        if (appendf(out, out_len, &off, ",\"lat\":%.7f,\"lng\":%.7f", snap->lat, snap->lng) != 0) {
            return -1;
        }
    }
    if (append(out, out_len, &off, "}") != 0) {
        return -1;
    }
    return (int)off;
}

int uplink_payload_build_host_reading_payload(const uplink_host_reading_t *reading, char *out,
                                              size_t out_len)
{
    if (!reading || !out || out_len < 32) {
        return -1;
    }
    size_t off = 0;
    out[0] = '\0';
    if (append(out, out_len, &off, "{\"key\":") != 0 ||
        append_json_str(out, out_len, &off, reading->key) != 0) {
        return -1;
    }
    if (appendf(out, out_len, &off, ",\"value\":%.4g,\"unit\":", reading->value) != 0) {
        return -1;
    }
    if (append_json_str(out, out_len, &off, reading->unit) != 0) {
        return -1;
    }
    if (appendf(out, out_len, &off, ",\"valid\":%s}", reading->valid ? "true" : "false") != 0) {
        return -1;
    }
    return (int)off;
}

static const char *obd_schema_or_default(const char *schema_id)
{
    if (schema_id != NULL && schema_id[0] != '\0') {
        return schema_id;
    }
    return UPLINK_SCHEMA_OBD;
}

static bool uplink_event_envelope_valid(const uplink_event_t *ev)
{
    return ev != NULL && ev->device_id[0] != '\0' && ev->node_id[0] != '\0' &&
           ev->schema_id != NULL && ev->schema_id[0] != '\0';
}

static int fill_event(uplink_event_t *ev, const char *device_id, const char *node_id,
                      const char *schema_id, uint64_t ts_ms, const char *payload_json)
{
    if (!ev || !device_id || !node_id || !schema_id || !payload_json) {
        return -1;
    }
    if (device_id[0] == '\0' || node_id[0] == '\0' || schema_id[0] == '\0') {
        return -1;
    }
    snprintf(ev->device_id, sizeof(ev->device_id), "%s", device_id);
    snprintf(ev->node_id, sizeof(ev->node_id), "%s", node_id);
    ev->schema_id = schema_id;
    ev->ts_ms = ts_ms;
    snprintf(ev->payload_json, sizeof(ev->payload_json), "%s", payload_json);
    return 0;
}

/*
 * Emit 0..N events. Over-capacity or malformed sub-events are skipped, never
 * fatal: one bad host reading must not drop the vehicle's OBD/GPS event.
 */
int uplink_events_from_snapshot(const uplink_snapshot_t *snap, const uplink_emit_ctx_t *ctx,
                                uplink_event_t *out, uint8_t max_out, uint8_t *count)
{
    if (!snap || !ctx || !out || !count || max_out == 0) {
        return -1;
    }
    *count = 0;

    if (ctx->include_obd && uplink_snapshot_has_fresh_pid(snap) && *count < max_out) {
        char payload[768];
        if (uplink_payload_build_obd_payload(snap, payload, sizeof(payload)) > 0 &&
            fill_event(&out[*count], snap->device_id, snap->node_id,
                       obd_schema_or_default(ctx->obd_schema_id), snap->ts_ms, payload) == 0) {
            (*count)++;
        }
    }

    if (ctx->include_gps && snap->gps_ok && *count < max_out) {
        char payload[128];
        char gps_dev[40];
        char gps_node[40];
        uplink_virtual_gps_ids(snap->device_id, snap->node_id, gps_dev, sizeof(gps_dev),
                               gps_node, sizeof(gps_node));
        if (uplink_payload_build_gps_payload(snap, payload, sizeof(payload)) > 0 &&
            fill_event(&out[*count], gps_dev, gps_node, UPLINK_SCHEMA_GPS, snap->ts_ms,
                       payload) == 0) {
            (*count)++;
        }
    }

    for (uint8_t hi = 0; hi < snap->host_count && hi < UPLINK_MAX_HOSTS; hi++) {
        const uplink_host_report_t *h = &snap->hosts[hi];
        const char *host_schema = uplink_schema_for_host(h->host_type_id);
        if (host_schema == NULL || h->device_id[0] == '\0') {
            continue;
        }
        char host_node[48];
        uplink_host_node_id(h->device_id, host_node, sizeof(host_node));
        for (uint8_t ri = 0; ri < h->reading_count && ri < UPLINK_MAX_HOST_READINGS; ri++) {
            const uplink_host_reading_t *r = &h->readings[ri];
            if (!r->valid || r->key[0] == '\0') {
                continue;
            }
            if (*count >= max_out) {
                return 0; /* Cap reached — send what we have; rest arrive next tick. */
            }
            char payload[256];
            /* Carrier tick time, not the host's own uptime clock — every event
             * in a batch must share one comparable time base. */
            if (uplink_payload_build_host_reading_payload(r, payload, sizeof(payload)) > 0 &&
                fill_event(&out[*count], h->device_id, host_node, host_schema, snap->ts_ms,
                           payload) == 0) {
                (*count)++;
            }
        }
    }

    return 0;
}

int uplink_event_serialize(const uplink_event_t *ev, char *out, size_t out_len)
{
    if (!uplink_event_envelope_valid(ev) || !out || out_len < 32) {
        return -1;
    }
    size_t off = 0;
    out[0] = '\0';
    if (append(out, out_len, &off, "{\"device_id\":") != 0 ||
        append_json_str(out, out_len, &off, ev->device_id) != 0) {
        return -1;
    }
    if (append(out, out_len, &off, ",\"node_id\":") != 0 ||
        append_json_str(out, out_len, &off, ev->node_id) != 0) {
        return -1;
    }
    if (append(out, out_len, &off, ",\"schemaId\":\"") != 0 ||
        append(out, out_len, &off, ev->schema_id) != 0 ||
        append(out, out_len, &off, "\",\"ts_ms\":") != 0) {
        return -1;
    }
    if (appendf(out, out_len, &off, "%llu,\"payload\":", (unsigned long long)ev->ts_ms) != 0) {
        return -1;
    }
    if (append(out, out_len, &off, ev->payload_json) != 0) {
        return -1;
    }
    if (append(out, out_len, &off, "}") != 0) {
        return -1;
    }
    return (int)off;
}

int uplink_event_serialize_queued(const uplink_event_t *ev, uint64_t queued_at_ms, char *out,
                                  size_t out_len)
{
    int n = uplink_event_serialize(ev, out, out_len);
    if (n < 0) {
        return -1;
    }
    size_t off = (size_t)n;
    if (off + 32 > out_len) {
        return -1;
    }
    /* Insert queued_at_ms before the closing brace. */
    if (off < 1 || out[off - 1] != '}') {
        return -1;
    }
    off--;
    out[off] = '\0';
    if (appendf(out, out_len, &off, ",\"queued_at_ms\":%llu}",
                (unsigned long long)queued_at_ms) != 0) {
        return -1;
    }
    return (int)off;
}

/*
 * Serializes in place (no temp buffer). Events that do not fit are dropped
 * rather than failing the POST — a full buffer must not cost the whole tick.
 */
int uplink_events_serialize_live(const uplink_event_t *evs, uint8_t count, char *out,
                                 size_t out_len)
{
    if (!evs || !out || count == 0 || out_len < 32) {
        return -1;
    }
    if (count == 1) {
        return uplink_event_serialize(&evs[0], out, out_len);
    }
    size_t off = 0;
    out[0] = '\0';
    if (append(out, out_len, &off, "[") != 0) {
        return -1;
    }
    size_t emitted = 0;
    for (uint8_t i = 0; i < count; i++) {
        size_t mark = off;
        if (emitted > 0 && append(out, out_len, &off, ",") != 0) {
            break;
        }
        /* Reserve one byte for the closing bracket. */
        if (off + 1 >= out_len) {
            off = mark;
            break;
        }
        int n = uplink_event_serialize(&evs[i], out + off, out_len - off - 1);
        if (n < 0) {
            off = mark;
            out[off] = '\0';
            continue;
        }
        off += (size_t)n;
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
  /* Root closing brace was replaced by ,"queued_at_ms":N} during enqueue. */
    size_t obj_len_local = (size_t)(found - line);
    if (obj_len_local < 2) {
        return -1;
    }
    /* Reject pre-envelope records written by older firmware: without schemaId
     * the backend would 400 the whole batch. */
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

int uplink_payload_build_batch(const char *events_blob, size_t n_events, char *out,
                               size_t out_len)
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
        if (len == 0) {
            continue;
        }
        consumed++;
        const char *event_obj = NULL;
        size_t event_len = 0;
        /* Skip an unparsable line instead of failing the drain — otherwise one
         * corrupt record (e.g. power loss mid-write) blocks the queue forever. */
        if (extract_queued_event(start, len, &event_obj, &event_len) != 0) {
            continue;
        }
        if (emitted > 0 && append(out, out_len, &off, ",") != 0) {
            return -1;
        }
        if (off + event_len + 2 > out_len) {
            return -1;
        }
        memcpy(out + off, event_obj, event_len);
        off += event_len;
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
