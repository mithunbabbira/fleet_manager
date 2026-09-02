#include "net_lte_time_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t net_lte_utc_datetime_to_epoch_ms(int year, int month, int day, int hour, int minute,
                                        int second)
{
    if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return -1;
    }
    int y = year;
    int m = month;
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m - 3) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    int64_t secs = days * 86400LL + (int64_t)hour * 3600LL + (int64_t)minute * 60LL + second;
    return secs * 1000LL;
}

bool net_lte_time_parse_cclk(const char *resp, int64_t *epoch_ms_utc_out)
{
    if (resp == NULL || epoch_ms_utc_out == NULL) {
        return false;
    }
    const char *p = strstr(resp, "+CCLK:");
    if (p == NULL) {
        return false;
    }
    p = strchr(p, '"');
    if (p == NULL) {
        return false;
    }
    p++;
    int yy = 0;
    int mo = 0;
    int dd = 0;
    int hh = 0;
    int mi = 0;
    int ss = 0;
    if (sscanf(p, "%d/%d/%d,%d:%d:%d", &yy, &mo, &dd, &hh, &mi, &ss) != 6) {
        return false;
    }
    if (yy < 100) {
        yy += 2000;
    }
    /* Quarter-hour offset, e.g. "+22" = IST. */
    int tz_q = NET_LTE_TZ_QUARTERS_IST;
    const char *tzp = strrchr(p, '+');
    if (tzp == NULL) {
        tzp = strrchr(p, '-');
        if (tzp != NULL) {
            tz_q = -atoi(tzp + 1);
        }
    } else {
        tz_q = atoi(tzp + 1);
    }
    int64_t epoch_ms = net_lte_utc_datetime_to_epoch_ms(yy, mo, dd, hh, mi, ss);
    if (epoch_ms < 0) {
        return false;
    }
    /* Local = UTC + tz_q * 15 min → UTC = local - offset */
    epoch_ms -= (int64_t)tz_q * 15LL * 60LL * 1000LL;
    *epoch_ms_utc_out = epoch_ms;
    return true;
}

int net_lte_format_ist(uint64_t epoch_ms_utc, char *out, size_t out_len)
{
    if (out == NULL || out_len < 20) {
        return -1;
    }
    /* IST = UTC + 5:30 */
    int64_t local_ms = (int64_t)epoch_ms_utc + (int64_t)NET_LTE_TZ_QUARTERS_IST * 15LL * 60LL * 1000LL;
    if (local_ms < 0) {
        return -1;
    }
    int64_t secs = local_ms / 1000LL;
    int ss = (int)(secs % 60);
    secs /= 60;
    int mi = (int)(secs % 60);
    secs /= 60;
    int hh = (int)(secs % 24);
    int64_t days = secs / 24;

    /* Civil from days since 1970-01-01 (Howard Hinnant algorithm). */
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int doe = (int)(days - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)(yoe + era * 400);
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int d = doy - (153 * mp + 2) / 5 + 1;
    int m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    return snprintf(out, out_len, "%04d-%02d-%02d %02d:%02d:%02d", y, m, d, hh, mi, ss);
}
