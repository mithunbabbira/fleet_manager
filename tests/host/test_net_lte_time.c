#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "net_lte_time_util.h"

int main(void)
{
    /* Known UTC: 2026-03-20 09:00:00 UTC = 1773997200000
     * IST wall: 2026-03-20 14:30:00 (UTC+5:30) */
    const int64_t utc_ms = 1773997200000LL;

    int64_t epoch = 0;
    assert(net_lte_time_parse_cclk("+CCLK: \"26/03/20,14:30:00+22\"\r\nOK\r\n", &epoch));
    assert(epoch == utc_ms);

    /* Operator omits TZ → assume IST (India-only product). */
    epoch = 0;
    assert(net_lte_time_parse_cclk("+CCLK: \"26/03/20,14:30:00\"\r\nOK\r\n", &epoch));
    assert(epoch == utc_ms);

    /* Explicit other TZ still converts correctly (UTC+0 → same as wall clock). */
    epoch = 0;
    assert(net_lte_time_parse_cclk("+CCLK: \"26/03/20,09:00:00+00\"\r\nOK\r\n", &epoch));
    assert(epoch == utc_ms);

    char ist[32];
    assert(net_lte_format_ist((uint64_t)utc_ms, ist, sizeof(ist)) > 0);
    assert(strcmp(ist, "2026-03-20 14:30:00") == 0);

    assert(net_lte_utc_datetime_to_epoch_ms(2026, 3, 20, 9, 0, 0) == utc_ms);
    assert(net_lte_utc_datetime_to_epoch_ms(1999, 1, 1, 0, 0, 0) < 0);
    assert(!net_lte_time_parse_cclk("ERROR", &epoch));
    assert(!net_lte_time_parse_cclk(NULL, &epoch));

    printf("test_net_lte_time: ok\n");
    return 0;
}
