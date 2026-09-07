#include "fleet_zb_epan.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect_true(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        fails++;
    }
}

int main(void)
{
    uint8_t le[8];
    char fmt[17];

    expect_true(fleet_zb_epan_parse("F1EE700000000001", le), "parse ok");
    expect_true(le[0] == 0x01 && le[1] == 0x00 && le[2] == 0x00 && le[3] == 0x00 &&
                    le[4] == 0x00 && le[5] == 0x70 && le[6] == 0xEE && le[7] == 0xF1,
                "LE bytes");

    fleet_zb_epan_format(le, fmt);
    expect_true(strcmp(fmt, "F1EE700000000001") == 0, "round-trip format");

    expect_true(!fleet_zb_epan_parse("short", le), "reject short");

    memset(le, 0xAA, sizeof(le));
    expect_true(!fleet_zb_epan_parse("F1EE", le), "reject short even-length");
    expect_true(memcmp(le, (uint8_t[8]){0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA}, 8) == 0,
                "out_le unchanged on short even-length reject");
    expect_true(!fleet_zb_epan_parse("F1EE70000000000G", le), "reject non-hex");
    expect_true(fleet_zb_epan_parse("0xF1EE700000000001", le), "accept 0x prefix");

    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    puts("ok");
    return 0;
}
