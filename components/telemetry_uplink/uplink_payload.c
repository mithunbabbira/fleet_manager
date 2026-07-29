#include "uplink_payload.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bool uplink_pid_is_fresh_ok(const uplink_pid_view_t *p)
{
    if (p == NULL) {
        return false;
    }
    return p->valid && p->ok && p->age_ms <= UPLINK_PID_FRESH_MS;
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

/* Minimal JSON string escape for device/node/protocol fields. */
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

static int append_pid_fields(char *out, size_t out_len, size_t *off,
                             const char *num_key, const char *raw_key,
                             const char *age_key, const char *ok_key,
                             const uplink_pid_view_t *p, bool raw_is_string)
{
    if (appendf(out, out_len, off, ",\"%s\":", num_key) != 0) {
        return -1;
    }
    if (uplink_pid_is_fresh_ok(p)) {
        if (appendf(out, out_len, off, "%.4g", p->value) != 0) {
            return -1;
        }
    } else if (append(out, out_len, off, "null") != 0) {
        return -1;
    }

    if (appendf(out, out_len, off, ",\"%s\":", raw_key) != 0) {
        return -1;
    }
    if (uplink_pid_is_fresh_ok(p)) {
        if (raw_is_string) {
            if (append_json_str(out, out_len, off, p->raw) != 0) {
                return -1;
            }
        } else if (append_json_str(out, out_len, off, p->raw) != 0) {
            return -1;
        }
    } else if (append(out, out_len, off, "null") != 0) {
        return -1;
    }

    if (appendf(out, out_len, off, ",\"%s\":", age_key) != 0) {
        return -1;
    }
    if (uplink_pid_is_fresh_ok(p)) {
        if (appendf(out, out_len, off, "%u", (unsigned)p->age_ms) != 0) {
            return -1;
        }
    } else if (append(out, out_len, off, "null") != 0) {
        return -1;
    }

    if (appendf(out, out_len, off, ",\"%s\":%s", ok_key,
                uplink_pid_is_fresh_ok(p) ? "true" : "false") != 0) {
        return -1;
    }
    return 0;
}

int uplink_payload_build(const uplink_snapshot_t *snap, char *out, size_t out_len)
{
    if (!snap || !out || out_len < 32) {
        return -1;
    }
    size_t off = 0;
    out[0] = '\0';

    if (append(out, out_len, &off, "{\"schemaId\":\"") != 0) {
        return -1;
    }
    if (append(out, out_len, &off, UPLINK_SCHEMA_ID) != 0) {
        return -1;
    }
    if (append(out, out_len, &off, "\",\"payload\":{") != 0) {
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
    if (append(out, out_len, &off, ",\"ble_peer_address\":") != 0 ||
        append_json_str(out, out_len, &off, snap->ble_peer_address) != 0) {
        return -1;
    }
    if (append(out, out_len, &off, ",\"adapter_name\":") != 0 ||
        append_json_str(out, out_len, &off, snap->adapter_name) != 0) {
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
    if (appendf(out, out_len, &off,
                ",\"uptime_seconds\":%u,\"ble_connected\":%s,\"elm_ready\":%s,\"poller_status\":",
                (unsigned)snap->uptime_seconds,
                snap->ble_connected ? "true" : "false",
                snap->elm_ready ? "true" : "false") != 0) {
        return -1;
    }
    if (append_json_str(out, out_len, &off,
                        snap->poller_status ? snap->poller_status : "paused") != 0) {
        return -1;
    }
    if (appendf(out, out_len, &off,
                ",\"cmds_ok\":%llu,\"cmds_fail\":%llu,\"ble_reconnects\":%llu,"
                "\"blocked_cmds\":%llu,\"telemetry_drops\":%llu",
                (unsigned long long)snap->cmds_ok,
                (unsigned long long)snap->cmds_fail,
                (unsigned long long)snap->ble_reconnects,
                (unsigned long long)snap->blocked_cmds,
                (unsigned long long)snap->telemetry_drops) != 0) {
        return -1;
    }

    /* Numeric keys match the Trafyn sample payload. */
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

    if (append(out, out_len, &off, ",\"source\":\"esp32_obd\"}}") != 0) {
        return -1;
    }
    return (int)off;
}
