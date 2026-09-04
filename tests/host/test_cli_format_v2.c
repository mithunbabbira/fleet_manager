#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cli_format.h"
#include "lte.h"

int main(void)
{
    char buf[128];
    lte_time_t t = {
        .time_ok = true,
        .epoch_ms_utc = 1773997200000ULL, /* 2026-03-20 09:00:00 UTC */
        .source = LTE_TIME_CCLK,
    };
    assert(cli_format_time_line(&t, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "ok=yes") != NULL);
    assert(strstr(buf, "src=cclk") != NULL);
    assert(strstr(buf, "ist=2026-03-20 14:30:00") != NULL);

    lte_time_t none = {0};
    assert(cli_format_time_line(&none, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "ok=no") != NULL);
    assert(strstr(buf, "src=none") != NULL);

    lte_gps_t g = {.gps_ok = true, .lat = 12.9716, .lng = 77.5946, .age_ms = 1500};
    assert(cli_format_gps_line(&g, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "ok=yes") != NULL);
    assert(strstr(buf, "lat=12.971600") != NULL);
    assert(strstr(buf, "lng=77.594600") != NULL);
    assert(strstr(buf, "age_ms=1500") != NULL);

    lte_gps_t bad = {0};
    assert(cli_format_gps_line(&bad, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "ok=no") != NULL);

    assert(cli_format_time_line(NULL, buf, sizeof(buf)) < 0);
    assert(cli_format_gps_line(&g, NULL, 8) < 0);

    printf("test_cli_format_v2: ok\n");
    return 0;
}
