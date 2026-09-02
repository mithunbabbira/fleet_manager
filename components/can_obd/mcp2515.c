#include "mcp2515.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "mcp2515";

/* Soft-SPI: ESP32-C6 has one GPSPI (SPI2), reserved for microSD.
 * MCP2515 uses GPIO bitbang on its own pins (defaults 21/22/23/20). */

/* SPI instructions */
#define CMD_RESET       0xC0
#define CMD_READ        0x03
#define CMD_WRITE       0x02
#define CMD_RTS_TXB0    0x81
#define CMD_BIT_MODIFY  0x05

/* Registers */
#define REG_CANSTAT   0x0E
#define REG_CANCTRL   0x0F
#define REG_CNF3      0x28
#define REG_CNF2      0x29
#define REG_CNF1      0x2A
#define REG_CANINTE   0x2B
#define REG_CANINTF   0x2C
#define REG_EFLG      0x2D
#define REG_TEC       0x1C
#define REG_REC       0x1D
#define REG_TXB0CTRL  0x30
#define REG_TXB0SIDH  0x31
#define REG_RXB0CTRL  0x60
#define REG_RXB0SIDH  0x61
#define REG_RXB1CTRL  0x70
#define REG_RXB1SIDH  0x71
#define REG_RXM0SIDH  0x20
#define REG_RXM1SIDH  0x24
#define REG_RXF0SIDH  0x00
#define REG_RXF1SIDH  0x04
#define REG_RXF2SIDH  0x08
#define REG_RXF3SIDH  0x10
#define REG_RXF4SIDH  0x14
#define REG_RXF5SIDH  0x18

#define INTF_RX0IF 0x01
#define INTF_RX1IF 0x02

#define TXB_TXREQ  0x08

static int s_sck = -1;
static int s_mosi = -1;
static int s_miso = -1;
static int s_cs = -1;

/* Bit timing table indexed by [xtal][bitrate]: {CNF1, CNF2, CNF3}. */
static const uint8_t k_timing[2][2][3] = {
    /* 8 MHz crystal */
    {
        {0x00, 0x90, 0x02}, /* 500 kbit */
        {0x00, 0xB1, 0x05}, /* 250 kbit */
    },
    /* 16 MHz crystal */
    {
        {0x00, 0xF0, 0x86}, /* 500 kbit */
        {0x41, 0xF1, 0x85}, /* 250 kbit */
    },
};

static int xtal_index(void)
{
#if CONFIG_CAN_OBD_XTAL_16MHZ
    return 1;
#else
    return 0;
#endif
}

static inline void cs_l(void) { gpio_set_level(s_cs, 0); }
static inline void cs_h(void) { gpio_set_level(s_cs, 1); }

static uint8_t soft_spi_byte(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 0; i < 8; i++) {
        gpio_set_level(s_mosi, (out & 0x80) ? 1 : 0);
        out <<= 1;
        gpio_set_level(s_sck, 1);
        in = (uint8_t)((in << 1) | (gpio_get_level(s_miso) & 1));
        gpio_set_level(s_sck, 0);
    }
    return in;
}

/** @brief CS-framed Mode 0 transfer (spi_clock_hz currently ignored — bitbang rate). */
static void soft_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    cs_l();
    for (size_t i = 0; i < n; i++) {
        uint8_t v = soft_spi_byte(tx ? tx[i] : 0xFF);
        if (rx) {
            rx[i] = v;
        }
    }
    cs_h();
}

static esp_err_t spi_cmd(uint8_t cmd)
{
    soft_spi_xfer(&cmd, NULL, 1);
    return ESP_OK;
}

static uint8_t read_reg(uint8_t reg)
{
    uint8_t tx[3] = {CMD_READ, reg, 0x00};
    uint8_t rx[3] = {0};
    soft_spi_xfer(tx, rx, 3);
    return rx[2];
}

static void read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    uint8_t tx[2 + 13] = {CMD_READ, reg};
    uint8_t rx[2 + 13] = {0};
    soft_spi_xfer(tx, rx, 2 + n);
    memcpy(buf, rx + 2, n);
}

static void write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[3] = {CMD_WRITE, reg, val};
    soft_spi_xfer(tx, NULL, 3);
}

static void write_regs(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tx[2 + 13] = {CMD_WRITE, reg};
    memcpy(tx + 2, buf, n);
    soft_spi_xfer(tx, NULL, 2 + n);
}

static void bit_modify(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t tx[4] = {CMD_BIT_MODIFY, reg, mask, val};
    soft_spi_xfer(tx, NULL, 4);
}

static void pack_id(uint32_t id, bool ext, uint8_t out[4])
{
    if (ext) {
        out[0] = (uint8_t)(id >> 21);
        out[1] = (uint8_t)(((id >> 18) & 0x07) << 5) | 0x08 |
                 (uint8_t)((id >> 16) & 0x03);
        out[2] = (uint8_t)(id >> 8);
        out[3] = (uint8_t)id;
    } else {
        out[0] = (uint8_t)(id >> 3);
        out[1] = (uint8_t)((id & 0x07) << 5);
        out[2] = 0;
        out[3] = 0;
    }
}

static bool unpack_rx_id(const uint8_t regs[4], uint32_t *id, bool *ext)
{
    if (regs[1] & 0x08) {
        *ext = true;
        *id = ((uint32_t)regs[0] << 21) |
              ((uint32_t)(regs[1] >> 5) << 18) |
              ((uint32_t)(regs[1] & 0x03) << 16) |
              ((uint32_t)regs[2] << 8) | regs[3];
    } else {
        *ext = false;
        *id = ((uint32_t)regs[0] << 3) | (regs[1] >> 5);
    }
    return true;
}

/**
 * @brief GPIO + RESET + verify config-mode CANSTAT (detect).
 */
esp_err_t mcp2515_init(int gpio_sck, int gpio_mosi, int gpio_miso, int gpio_cs,
                       int spi_clock_hz)
{
    (void)spi_clock_hz;
    s_sck = gpio_sck;
    s_mosi = gpio_mosi;
    s_miso = gpio_miso;
    s_cs = gpio_cs;

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << s_sck) | (1ULL << s_mosi) | (1ULL << s_cs),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << s_miso),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    ESP_ERROR_CHECK(gpio_config(&in));
    gpio_set_level(s_cs, 1);
    gpio_set_level(s_sck, 0);
    gpio_set_level(s_mosi, 0);

    ESP_LOGI(TAG, "soft-SPI SCK=%d MOSI=%d MISO=%d CS=%d", s_sck, s_mosi, s_miso, s_cs);

    ESP_ERROR_CHECK(spi_cmd(CMD_RESET));
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t canstat = read_reg(REG_CANSTAT);
    if ((canstat & 0xE0) != 0x80) {
        ESP_LOGE(TAG, "not responding (CANSTAT=0x%02X); check wiring/power", canstat);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "detected (CANSTAT=0x%02X)", canstat);
    return ESP_OK;
}

/**
 * @brief Bit timing, masks, all RXF0–RXF5 filters, Normal mode.
 * @warning Unused RXFn left at reset defaults — program all filters (C1).
 */
esp_err_t mcp2515_configure(mcp_bitrate_t bitrate, bool ext,
                            uint32_t filter_id, uint32_t filter_mask)
{
    ESP_ERROR_CHECK(spi_cmd(CMD_RESET));
    vTaskDelay(pdMS_TO_TICKS(10));

    const uint8_t *cnf = k_timing[xtal_index()][bitrate];
    write_reg(REG_CNF1, cnf[0]);
    write_reg(REG_CNF2, cnf[1]);
    write_reg(REG_CNF3, cnf[2]);

    uint8_t packed[4];
    pack_id(filter_mask, ext, packed);
    write_regs(REG_RXM0SIDH, packed, 4);
    write_regs(REG_RXM1SIDH, packed, 4);
    /* Program ALL acceptance filters — unused RXFn left at reset 0 would match
     * std IDs 0x000–0x007 under a typical OBD mask (RXB0=F0|F1, RXB1=F2–F5). */
    pack_id(filter_id, ext, packed);
    write_regs(REG_RXF0SIDH, packed, 4);
    write_regs(REG_RXF1SIDH, packed, 4);
    write_regs(REG_RXF2SIDH, packed, 4);
    write_regs(REG_RXF3SIDH, packed, 4);
    write_regs(REG_RXF4SIDH, packed, 4);
    write_regs(REG_RXF5SIDH, packed, 4);

    write_reg(REG_RXB0CTRL, 0x04); /* filtered, rollover to RXB1 */
    write_reg(REG_RXB1CTRL, 0x00); /* filtered */
    write_reg(REG_CANINTE, 0x00);  /* polled operation */

    write_reg(REG_CANCTRL, 0x00); /* Normal mode */
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t mode = read_reg(REG_CANSTAT) >> 5;
    if (mode != 0) {
        ESP_LOGE(TAG, "failed to enter Normal mode (OPMOD=%u)", mode);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** @brief Load TXB0 + RTS (does not wait for ACK). */
esp_err_t mcp2515_send(const mcp_can_frame_t *frame)
{
    if (frame == NULL || frame->dlc > 8) {
        return ESP_ERR_INVALID_ARG;
    }

    bit_modify(REG_TXB0CTRL, TXB_TXREQ, 0x00);
    bit_modify(REG_CANINTF, 0x04, 0x00);

    uint8_t buf[13];
    pack_id(frame->id, frame->ext, buf);
    buf[4] = frame->dlc;
    memcpy(buf + 5, frame->data, frame->dlc);
    write_regs(REG_TXB0SIDH, buf, 5 + frame->dlc);
    return spi_cmd(CMD_RTS_TXB0);
}

/**
 * @brief TXREQ clear — finished, aborted, or bus-off cleared request (not pure ACK).
 */
bool mcp2515_tx_done(void)
{
    return (read_reg(REG_TXB0CTRL) & TXB_TXREQ) == 0;
}

void mcp2515_tx_abort(void)
{
    bit_modify(REG_TXB0CTRL, TXB_TXREQ, 0x00);
}

bool mcp2515_receive(mcp_can_frame_t *frame)
{
    if (frame == NULL) {
        return false;
    }
    uint8_t intf = read_reg(REG_CANINTF);
    uint8_t which;
    uint8_t base;

    if (intf & INTF_RX0IF) {
        which = INTF_RX0IF;
        base = REG_RXB0SIDH;
    } else if (intf & INTF_RX1IF) {
        which = INTF_RX1IF;
        base = REG_RXB1SIDH;
    } else {
        return false;
    }

    uint8_t raw[13];
    read_regs(base, raw, sizeof(raw));
    unpack_rx_id(raw, &frame->id, &frame->ext);
    frame->dlc = raw[4] & 0x0F;
    if (frame->dlc > 8) {
        frame->dlc = 8;
    }
    memcpy(frame->data, raw + 5, frame->dlc);
    bit_modify(REG_CANINTF, which, 0x00);
    return true;
}

void mcp2515_read_errors(uint8_t *tec, uint8_t *rec, uint8_t *eflg)
{
    if (tec) {
        *tec = read_reg(REG_TEC);
    }
    if (rec) {
        *rec = read_reg(REG_REC);
    }
    if (eflg) {
        *eflg = read_reg(REG_EFLG);
    }
}
