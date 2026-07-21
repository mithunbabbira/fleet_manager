#include "synthetic_data.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_first_snapshot(void)
{
    synthetic_snapshot_t s;
    synthetic_snapshot_generate(0, 1000, &s);

    assert(s.sequence == 0);
    assert(s.ts_ms == 1000);
    assert(s.uptime_s == 1);
    assert(s.rpm >= 750.0 && s.rpm <= 2500.0);
    assert(s.speed_kmh <= 80);
    assert(s.coolant_c >= 85 && s.coolant_c <= 95);
    assert(s.throttle_pct >= 10.0 && s.throttle_pct <= 60.0);
    assert(s.voltage_v >= 13.5 && s.voltage_v <= 14.2);
    assert(strncmp(s.rpm_raw, "410C", 4) == 0);
    assert(strncmp(s.speed_raw, "410D", 4) == 0);
    assert(strncmp(s.coolant_raw, "4105", 4) == 0);
    assert(strncmp(s.throttle_raw, "4111", 4) == 0);
}

static void test_repeatable_and_changing(void)
{
    synthetic_snapshot_t a;
    synthetic_snapshot_t b;
    synthetic_snapshot_t again;
    synthetic_snapshot_generate(5, 6000, &a);
    synthetic_snapshot_generate(6, 7000, &b);
    synthetic_snapshot_generate(5, 6000, &again);

    assert(memcmp(&a, &again, sizeof(a)) == 0);
    assert(a.speed_kmh != b.speed_kmh || fabs(a.rpm - b.rpm) > 0.01);
}

int main(void)
{
    test_first_snapshot();
    test_repeatable_and_changing();
    puts("synthetic_data tests passed");
    return 0;
}
