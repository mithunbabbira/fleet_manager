#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "obd_codec.h"

int main(void)
{
    obd_decoded_t d;

    /* 41 0C 1A F8 → RPM = ((0x1A<<8)|0xF8)/4 = 1726 */
    assert(obd_codec_decode_mode01("41 0C 1A F8", 0x0C, &d));
    assert(d.ok);
    assert(fabs(d.value - 1726.0) < 0.01);
    assert(strcmp(d.unit, "rpm") == 0);

    /* 41 0D 32 → speed 50 km/h */
    assert(obd_codec_decode_mode01("410D32", 0x0D, &d));
    assert(fabs(d.value - 50.0) < 0.01);

    /* 41 05 64 → coolant 60 C (A-40) */
    assert(obd_codec_decode_mode01("41 05 64", 0x05, &d));
    assert(fabs(d.value - 60.0) < 0.01);

    char dtcs[4][6];
    int n = obd_codec_parse_dtcs("43 01 33 00 00 00 00", dtcs, 4);
    assert(n == 1);
    assert(strcmp(dtcs[0], "P0133") == 0);

    char vin[32];
    assert(obd_codec_parse_vin(
        "014\r0: 49 02 01 31 47 31\r1: 4A 43 35 34 34 34 52\r2: 37 32 35 32 33 36 37",
        vin, sizeof(vin)));
    assert(strlen(vin) == 17);

    assert(obd_codec_decode_named("rpm", "41 0C 1A F8", &d));
    assert(fabs(d.value - 1726.0) < 0.01);

    printf("test_obd_codec: PASS\n");
    return 0;
}
