#pragma once
#include "profile_store.h"

/* Built-in profiles seeded into NVS on first boot (design spec section 6). */
static const obd_profile_t k_builtin_profiles[] = {
    {
        .name = "fleet_basic",
        .init_at = {"ATZ", "ATE0", "ATL0", "ATS0", "ATH0", "ATSP0"},
        .init_at_count = 6,
        .items = {
            {.cmd = "010C", .interval_ms = 200, .decode = "rpm"},
            {.cmd = "010D", .interval_ms = 200, .decode = "speed"},
            {.cmd = "0105", .interval_ms = 2000, .decode = "coolant_c"},
            {.cmd = "ATRV", .interval_ms = 5000, .decode = "voltage"},
        },
        .item_count = 4,
    },
    {
        .name = "diagnostics",
        .init_at = {"ATZ", "ATE0", "ATL0", "ATS0", "ATH0", "ATSP0"},
        .init_at_count = 6,
        .items = {
            {.cmd = "010C", .interval_ms = 1000, .decode = "rpm"},
            {.cmd = "010D", .interval_ms = 1000, .decode = "speed"},
            {.cmd = "0105", .interval_ms = 5000, .decode = "coolant_c"},
            {.cmd = "0111", .interval_ms = 2000, .decode = "throttle_pct"},
            {.cmd = "ATRV", .interval_ms = 5000, .decode = "voltage"},
            {.cmd = "03", .interval_ms = 10000, .decode = "dtc"},
            {.cmd = "0902", .interval_ms = 30000, .decode = "vin"},
        },
        .item_count = 7,
    },
};

#define BUILTIN_PROFILE_COUNT (sizeof(k_builtin_profiles) / sizeof(k_builtin_profiles[0]))
