#include "cli_format.h"
#include "lte_time.h"

#include <stdio.h>

static const char *time_src_name(lte_time_source_t s)
{
    switch (s) {
    case LTE_TIME_CCLK:
        return "cclk";
    case LTE_TIME_GPS:
        return "gps";
    default:
        return "none";
    }
}

int cli_format_time_line(const lte_time_t *t, char *out, size_t out_len)
{
    if (t == NULL || out == NULL || out_len < 8) {
        return -1;
    }
    char ist[32] = "-";
    if (t->time_ok && t->epoch_ms_utc > 0) {
        if (lte_format_ist(t->epoch_ms_utc, ist, sizeof(ist)) < 0) {
            snprintf(ist, sizeof(ist), "-");
        }
    }
    int n = snprintf(out, out_len, "time: ok=%s src=%s utc_ms=%llu ist=%s",
                     t->time_ok ? "yes" : "no", time_src_name(t->source),
                     (unsigned long long)t->epoch_ms_utc, ist);
    return (n < 0 || (size_t)n >= out_len) ? -1 : n;
}

int cli_format_gps_line(const lte_gps_t *g, char *out, size_t out_len)
{
    if (g == NULL || out == NULL || out_len < 8) {
        return -1;
    }
    int n;
    if (g->gps_ok) {
        n = snprintf(out, out_len, "gps: ok=yes lat=%.6f lng=%.6f age_ms=%u", g->lat,
                     g->lng, (unsigned)g->age_ms);
    } else {
        n = snprintf(out, out_len, "gps: ok=no lat=0.000000 lng=0.000000 age_ms=%u",
                     (unsigned)g->age_ms);
    }
    return (n < 0 || (size_t)n >= out_len) ? -1 : n;
}
