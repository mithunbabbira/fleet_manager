#include "uplink_obd.h"

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

bool uplink_obd_has_fresh_pid(const uplink_obd_snapshot_t *snap)
{
    if (snap == NULL) {
        return false;
    }
    return uplink_pid_is_fresh_ok(&snap->rpm) || uplink_pid_is_fresh_ok(&snap->speed) ||
           uplink_pid_is_fresh_ok(&snap->coolant) || uplink_pid_is_fresh_ok(&snap->throttle);
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

int uplink_build_obd_payload(const uplink_obd_snapshot_t *snap, char *out, size_t out_len)
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
    if (appendf(out, out_len, &off, ",\"cmds_ok\":%llu,\"cmds_fail\":%llu",
                (unsigned long long)snap->cmds_ok, (unsigned long long)snap->cmds_fail) != 0) {
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
    /* voltage not polled in M4 thin poller — always false */
    if (append(out, out_len, &off, ",\"voltage_ok\":false,\"source\":\"esp32_obd\"}") != 0) {
        return -1;
    }
    return (int)off;
}
