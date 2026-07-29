#include "mcp2515.h"

#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "mcp2515";

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
#define REG_RXF2SIDH  0x08

#define INTF_RX0IF 0x01
#define INTF_RX1IF 0x02

#define TXB_TXREQ  0x08

static spi_device_handle_t s_spi;

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

static esp_err_t spi_cmd(uint8_t cmd)
{
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    return spi_device_transmit(s_spi, &t);
}

static uint8_t read_reg(uint8_t reg)
{
    uint8_t tx[3] = {CMD_READ, reg, 0x00};
    uint8_t rx[3] = {0};
    spi_transaction_t t = {.length = 24, .tx_buffer = tx, .rx_buffer = rx};
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    return rx[2];
}

static void read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    uint8_t tx[2 + 13] = {CMD_READ, reg};
    uint8_t rx[2 + 13] = {0};
    spi_transaction_t t = {.length = (2 + n) * 8, .tx_buffer = tx, .rx_buffer = rx};
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    memcpy(buf, rx + 2, n);
}

static void write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[3] = {CMD_WRITE, reg, val};
    spi_transaction_t t = {.length = 24, .tx_buffer = tx};
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
}

static void write_regs(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tx[2 + 13] = {CMD_WRITE, reg};
    memcpy(tx + 2, buf, n);
    spi_transaction_t t = {.length = (2 + n) * 8, .tx_buffer = tx};
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
}

static void bit_modify(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t tx[4] = {CMD_BIT_MODIFY, reg, mask, val};
    spi_transaction_t t = {.length = 32, .tx_buffer = tx};
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
}

/* Pack a CAN id into SIDH/SIDL/EID8/EID0 (id registers, filters and masks). */
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
    if (regs[1] & 0x08) { /* IDE bit in RXBnSIDL */
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

esp_err_t mcp2515_init(int gpio_sck, int gpio_mosi, int gpio_miso, int gpio_cs,
                       int spi_clock_hz)
{
    spi_bus_config_t bus = {
        .sclk_io_num = gpio_sck,
        .mosi_io_num = gpio_mosi,
        .miso_io_num = gpio_miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = spi_clock_hz,
        .mode = 0,
        .spics_io_num = gpio_cs,
        .queue_size = 4,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        return err;
    }

    /* Smoke check: after reset CANSTAT must report Configuration mode. */
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

esp_err_t mcp2515_configure(mcp_bitrate_t bitrate, bool ext,
                            uint32_t filter_id, uint32_t filter_mask)
{
    ESP_ERROR_CHECK(spi_cmd(CMD_RESET));
    vTaskDelay(pdMS_TO_TICKS(10));

    const uint8_t *cnf = k_timing[xtal_index()][bitrate];
    write_reg(REG_CNF1, cnf[0]);
    write_reg(REG_CNF2, cnf[1]);
    write_reg(REG_CNF3, cnf[2]);

    /* Same filter on both RX buffers; masks and filters share id packing. */
    uint8_t packed[4];
    pack_id(filter_mask, ext, packed);
    write_regs(REG_RXM0SIDH, packed, 4);
    write_regs(REG_RXM1SIDH, packed, 4);
    pack_id(filter_id, ext, packed);
    write_regs(REG_RXF0SIDH, packed, 4);
    write_regs(REG_RXF2SIDH, packed, 4);

    write_reg(REG_RXB0CTRL, 0x04); /* filtered, rollover to RXB1 */
    write_reg(REG_RXB1CTRL, 0x00); /* filtered */
    write_reg(REG_CANINTE, 0x00);  /* polled operation */

    write_reg(REG_CANCTRL, 0x00);  /* Normal mode */
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t mode = read_reg(REG_CANSTAT) >> 5;
    if (mode != 0) {
        ESP_LOGE(TAG, "failed to enter Normal mode (OPMOD=%u)", mode);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mcp2515_send(const mcp_can_frame_t *frame)
{
    if (frame == NULL || frame->dlc > 8) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Clear any pending request + its interrupt flag first. */
    bit_modify(REG_TXB0CTRL, TXB_TXREQ, 0x00);
    bit_modify(REG_CANINTF, 0x04, 0x00);

    uint8_t buf[13];
    pack_id(frame->id, frame->ext, buf);
    buf[4] = frame->dlc;
    memcpy(buf + 5, frame->data, frame->dlc);
    write_regs(REG_TXB0SIDH, buf, 5 + frame->dlc);
    return spi_cmd(CMD_RTS_TXB0);
}

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
