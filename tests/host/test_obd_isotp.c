#include "obd_isotp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_build_sf(void)
{
    uint8_t f[8];

    assert(obd_isotp_build_sf("010C", f) == 0);
    assert(f[0] == 0x02 && f[1] == 0x01 && f[2] == 0x0C);
    assert(f[3] == 0x00 && f[7] == 0x00); /* padded */

    assert(obd_isotp_build_sf("03", f) == 0);
    assert(f[0] == 0x01 && f[1] == 0x03);

    assert(obd_isotp_build_sf("0902", f) == 0);
    assert(f[0] == 0x02 && f[1] == 0x09 && f[2] == 0x02);

    assert(obd_isotp_build_sf("", f) == -1);
    assert(obd_isotp_build_sf("0", f) == -1);          /* odd length */
    assert(obd_isotp_build_sf("01ZZ", f) == -1);       /* bad hex */
    assert(obd_isotp_build_sf("0102030405060708", f) == -1); /* > 7 bytes */
}

static void test_single_frame_rx(void)
{
    obd_isotp_rx_t rx;
    obd_isotp_rx_reset(&rx);

    /* 41 00 98 3A A0 13 (real reply captured from the car) */
    uint8_t sf[8] = {0x06, 0x41, 0x00, 0x98, 0x3A, 0xA0, 0x13, 0xFF};
    assert(obd_isotp_rx_feed(&rx, 0x7E8, sf, 8) == OBD_ISOTP_RX_COMPLETE);

    char hex[64];
    assert(obd_isotp_payload_hex(&rx, hex, sizeof(hex)) == 12);
    assert(strcmp(hex, "4100983AA013") == 0);
}

static void test_multi_frame_rx(void)
{
    obd_isotp_rx_t rx;
    obd_isotp_rx_reset(&rx);

    /* VIN-style: total 20 bytes = FF(6) + CF(7) + CF(7) */
    uint8_t ff[8] = {0x10, 0x14, 0x49, 0x02, 0x01, 0x41, 0x42, 0x43};
    assert(obd_isotp_rx_feed(&rx, 0x7E8, ff, 8) == OBD_ISOTP_RX_NEED_FC);

    uint8_t cf1[8] = {0x21, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A};
    assert(obd_isotp_rx_feed(&rx, 0x7E8, cf1, 8) == OBD_ISOTP_RX_IN_PROGRESS);

    uint8_t cf2[8] = {0x22, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51};
    assert(obd_isotp_rx_feed(&rx, 0x7E8, cf2, 8) == OBD_ISOTP_RX_COMPLETE);

    char hex[64];
    assert(obd_isotp_payload_hex(&rx, hex, sizeof(hex)) == 40);
    assert(strcmp(hex, "4902014142434445464748494A4B4C4D4E4F5051") == 0);
}

static void test_multi_frame_ignores_other_ecu(void)
{
    obd_isotp_rx_t rx;
    obd_isotp_rx_reset(&rx);

    uint8_t ff[8] = {0x10, 0x14, 0x49, 0x02, 0x01, 0x41, 0x42, 0x43};
    assert(obd_isotp_rx_feed(&rx, 0x7E8, ff, 8) == OBD_ISOTP_RX_NEED_FC);

    /* Frame from a different ECU mid-transfer must be ignored. */
    uint8_t other[8] = {0x06, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert(obd_isotp_rx_feed(&rx, 0x7E9, other, 8) == OBD_ISOTP_RX_IGNORED);

    uint8_t cf1[8] = {0x21, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A};
    assert(obd_isotp_rx_feed(&rx, 0x7E8, cf1, 8) == OBD_ISOTP_RX_IN_PROGRESS);
}

static void test_sequence_error(void)
{
    obd_isotp_rx_t rx;
    obd_isotp_rx_reset(&rx);

    uint8_t ff[8] = {0x10, 0x14, 0x49, 0x02, 0x01, 0x41, 0x42, 0x43};
    assert(obd_isotp_rx_feed(&rx, 0x7E8, ff, 8) == OBD_ISOTP_RX_NEED_FC);

    /* Wrong sequence number (2 instead of 1). */
    uint8_t bad[8] = {0x22, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A};
    assert(obd_isotp_rx_feed(&rx, 0x7E8, bad, 8) == OBD_ISOTP_RX_ERROR);
    assert(!rx.in_progress); /* state reset */
}

static void test_flow_control(void)
{
    uint8_t fc[8];
    obd_isotp_build_fc(fc);
    assert(fc[0] == 0x30 && fc[1] == 0x00 && fc[2] == 0x00);
}

int main(void)
{
    test_build_sf();
    test_single_frame_rx();
    test_multi_frame_rx();
    test_multi_frame_ignores_other_ecu();
    test_sequence_error();
    test_flow_control();
    printf("test_obd_isotp: all tests passed\n");
    return 0;
}
