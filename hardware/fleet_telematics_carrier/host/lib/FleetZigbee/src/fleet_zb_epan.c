#include "fleet_zb_epan.h"

#include <string.h>

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool fleet_zb_epan_parse(const char *hex16, uint8_t out_le[8])
{
    if (hex16[0] == '0' && (hex16[1] == 'x' || hex16[1] == 'X')) {
        hex16 += 2;
    }

    if (strlen(hex16) != 16) {
        return false;
    }

    uint8_t msb_first[8];
    for (int i = 0; i < 8; i++) {
        int hi = hex_nibble(hex16[i * 2]);
        int lo = hex_nibble(hex16[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        msb_first[i] = (uint8_t)((hi << 4) | lo);
    }

    if (hex16[16] != '\0') {
        return false;
    }

    for (int i = 0; i < 8; i++) {
        out_le[i] = msb_first[7 - i];
    }
    return true;
}

void fleet_zb_epan_format(const uint8_t in_le[8], char out[17])
{
    static const char hex[] = "0123456789ABCDEF";

    for (int i = 0; i < 8; i++) {
        uint8_t b = in_le[7 - i];
        out[i * 2] = hex[b >> 4];
        out[i * 2 + 1] = hex[b & 0x0F];
    }
    out[16] = '\0';
}
