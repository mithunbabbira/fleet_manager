#include "obd_isotp.h"

#include <string.h>

#define PCI_TYPE_SF 0x0
#define PCI_TYPE_FF 0x1
#define PCI_TYPE_CF 0x2
#define PCI_TYPE_FC 0x3

#define FRAME_PAD 0x00

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

int obd_isotp_build_sf(const char *cmd_hex, uint8_t out[8])
{
    if (cmd_hex == NULL || out == NULL) {
        return -1;
    }

    size_t hex_len = strlen(cmd_hex);
    if (hex_len == 0 || (hex_len % 2) != 0 || hex_len > 14) {
        return -1;
    }

    size_t n = hex_len / 2;
    memset(out, FRAME_PAD, 8);
    out[0] = (uint8_t)((PCI_TYPE_SF << 4) | n);

    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(cmd_hex[i * 2]);
        int lo = hex_nibble(cmd_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[1 + i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

void obd_isotp_rx_reset(obd_isotp_rx_t *rx)
{
    memset(rx, 0, sizeof(*rx));
}

obd_isotp_rx_status_t obd_isotp_rx_feed(obd_isotp_rx_t *rx, uint32_t can_id,
                                        const uint8_t *data, uint8_t dlc)
{
    if (rx == NULL || data == NULL || dlc == 0) {
        return OBD_ISOTP_RX_IGNORED;
    }

    uint8_t pci_type = data[0] >> 4;

    if (rx->in_progress) {
        if (can_id != rx->src_id) {
            return OBD_ISOTP_RX_IGNORED; /* another ECU chattering mid-transfer */
        }
        if (pci_type != PCI_TYPE_CF) {
            obd_isotp_rx_reset(rx);
            return OBD_ISOTP_RX_ERROR;
        }
        uint8_t sn = data[0] & 0x0F;
        if (sn != rx->next_sn) {
            obd_isotp_rx_reset(rx);
            return OBD_ISOTP_RX_ERROR;
        }
        rx->next_sn = (uint8_t)((rx->next_sn + 1) & 0x0F);

        size_t remaining = rx->expected_len - rx->got_len;
        size_t chunk = (size_t)dlc - 1;
        if (chunk > remaining) {
            chunk = remaining;
        }
        if (rx->got_len + chunk > sizeof(rx->buf)) {
            obd_isotp_rx_reset(rx);
            return OBD_ISOTP_RX_ERROR;
        }
        memcpy(rx->buf + rx->got_len, data + 1, chunk);
        rx->got_len += chunk;

        if (rx->got_len >= rx->expected_len) {
            rx->in_progress = false;
            return OBD_ISOTP_RX_COMPLETE;
        }
        return OBD_ISOTP_RX_IN_PROGRESS;
    }

    if (pci_type == PCI_TYPE_SF) {
        size_t n = data[0] & 0x0F;
        if (n == 0 || n > 7 || n > (size_t)(dlc - 1)) {
            return OBD_ISOTP_RX_IGNORED;
        }
        memcpy(rx->buf, data + 1, n);
        rx->expected_len = n;
        rx->got_len = n;
        rx->src_id = can_id;
        return OBD_ISOTP_RX_COMPLETE;
    }

    if (pci_type == PCI_TYPE_FF) {
        if (dlc < 8) {
            return OBD_ISOTP_RX_IGNORED;
        }
        size_t total = ((size_t)(data[0] & 0x0F) << 8) | data[1];
        if (total < 8 || total > sizeof(rx->buf)) {
            return OBD_ISOTP_RX_IGNORED; /* too big for OBD purposes */
        }
        memcpy(rx->buf, data + 2, 6);
        rx->expected_len = total;
        rx->got_len = 6;
        rx->next_sn = 1;
        rx->in_progress = true;
        rx->src_id = can_id;
        return OBD_ISOTP_RX_NEED_FC;
    }

    return OBD_ISOTP_RX_IGNORED;
}

int obd_isotp_payload_hex(const obd_isotp_rx_t *rx, char *out, size_t out_len)
{
    static const char digits[] = "0123456789ABCDEF";

    if (rx == NULL || out == NULL || out_len < rx->got_len * 2 + 1) {
        return -1;
    }
    for (size_t i = 0; i < rx->got_len; i++) {
        out[i * 2] = digits[rx->buf[i] >> 4];
        out[i * 2 + 1] = digits[rx->buf[i] & 0x0F];
    }
    out[rx->got_len * 2] = '\0';
    return (int)(rx->got_len * 2);
}

void obd_isotp_build_fc(uint8_t out[8])
{
    memset(out, FRAME_PAD, 8);
    out[0] = (uint8_t)(PCI_TYPE_FC << 4); /* ClearToSend */
    out[1] = 0x00;                        /* block size: unlimited */
    out[2] = 0x00;                        /* STmin: no delay */
}
