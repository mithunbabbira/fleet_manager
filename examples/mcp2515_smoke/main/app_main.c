/*
 * MCP2515 OBD-II probe for ESP32-C6 Super Mini.
 *
 * Stage 1 (bench):  SPI smoke test — reset, register readback.
 * Stage 2 (in car): configure ISO 15765-4 CAN 11-bit / 500 kbit (8 MHz xtal),
 *                   send OBD "0100" (supported PIDs) to 0x7DF and print every
 *                   response from 0x7E8..0x7EF, plus TX/error diagnostics.
 *
 * Wiring: GPIO21 SCK | GPIO22 MOSI | GPIO23 MISO | GPIO20 CS | GPIO14 INT
 */

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mcp2515_probe";

#define PIN_SCK   21
#define PIN_MOSI  22
#define PIN_MISO  23
#define PIN_CS    20
#define PIN_INT   14

/* MCP2515 SPI instructions */
#define MCP_CMD_RESET       0xC0
#define MCP_CMD_READ        0x03
#define MCP_CMD_WRITE       0x02
#define MCP_CMD_RTS_TXB0    0x81
#define MCP_CMD_BIT_MODIFY  0x05

/* MCP2515 registers */
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

/* CANINTF flags */
#define INTF_RX0IF  0x01
#define INTF_RX1IF  0x02

/* 500 kbit/s @ 8 MHz crystal: 8 TQ/bit, sample point 62.5% */
#define CNF1_500K_8MHZ  0x00
#define CNF2_500K_8MHZ  0x90
#define CNF3_500K_8MHZ  0x02

#define OBD_REQ_ID   0x7DF
#define OBD_RESP_LO  0x7E8
#define OBD_RESP_HI  0x7EF

static spi_device_handle_t s_spi;

static esp_err_t mcp_cmd(uint8_t cmd)
{
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    return spi_device_transmit(s_spi, &t);
}

static uint8_t mcp_read_reg(uint8_t reg)
{
    uint8_t tx[3] = {MCP_CMD_READ, reg, 0x00};
    uint8_t rx[3] = {0};
    spi_transaction_t t = {.length = 24, .tx_buffer = tx, .rx_buffer = rx};
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    return rx[2];
}

static void mcp_read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    /* Sequential read: address auto-increments after each byte. */
    uint8_t tx[2 + 16] = {MCP_CMD_READ, reg};
    uint8_t rx[2 + 16] = {0};
    spi_transaction_t t = {
        .length = (2 + n) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
    for (size_t i = 0; i < n; i++) {
        buf[i] = rx[2 + i];
    }
}

static void mcp_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[3] = {MCP_CMD_WRITE, reg, val};
    spi_transaction_t t = {.length = 24, .tx_buffer = tx};
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
}

static void mcp_write_regs(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tx[2 + 16] = {MCP_CMD_WRITE, reg};
    for (size_t i = 0; i < n; i++) {
        tx[2 + i] = buf[i];
    }
    spi_transaction_t t = {.length = (2 + n) * 8, .tx_buffer = tx};
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
}

static void mcp_bit_modify(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t tx[4] = {MCP_CMD_BIT_MODIFY, reg, mask, val};
    spi_transaction_t t = {.length = 32, .tx_buffer = tx};
    ESP_ERROR_CHECK(spi_device_transmit(s_spi, &t));
}

static bool smoke_test(void)
{
    ESP_ERROR_CHECK(mcp_cmd(MCP_CMD_RESET));
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t canstat = mcp_read_reg(REG_CANSTAT);
    uint8_t canctrl = mcp_read_reg(REG_CANCTRL);
    mcp_write_reg(REG_CNF1, 0x55);
    uint8_t readback = mcp_read_reg(REG_CNF1);

    bool ok = ((canstat & 0xE0) == 0x80) && (readback == 0x55) &&
              (canctrl != 0x00) && (canctrl != 0xFF);
    ESP_LOGI(TAG, "smoke: CANSTAT=0x%02X CANCTRL=0x%02X readback=0x%02X -> %s",
             canstat, canctrl, readback, ok ? "PASS" : "FAIL");
    return ok;
}

static bool can_configure_500k(void)
{
    ESP_ERROR_CHECK(mcp_cmd(MCP_CMD_RESET));
    vTaskDelay(pdMS_TO_TICKS(10));

    mcp_write_reg(REG_CNF1, CNF1_500K_8MHZ);
    mcp_write_reg(REG_CNF2, CNF2_500K_8MHZ);
    mcp_write_reg(REG_CNF3, CNF3_500K_8MHZ);

    /* Accept every frame (filter in software); RXB0 rolls over into RXB1. */
    mcp_write_reg(REG_RXB0CTRL, 0x64);
    mcp_write_reg(REG_RXB1CTRL, 0x60);
    mcp_write_reg(REG_CANINTE, 0x00);  /* polling, no INT pin use yet */

    /* Normal mode */
    mcp_write_reg(REG_CANCTRL, 0x00);
    vTaskDelay(pdMS_TO_TICKS(5));
    uint8_t mode = mcp_read_reg(REG_CANSTAT) >> 5;
    if (mode != 0) {
        ESP_LOGE(TAG, "failed to enter Normal mode (OPMOD=%u)", mode);
        return false;
    }
    ESP_LOGI(TAG, "CAN configured: 500 kbit/s, 11-bit, Normal mode");
    return true;
}

static void send_obd_0100(void)
{
    /* Abort any stuck previous TX, clear its flag. */
    mcp_bit_modify(REG_TXB0CTRL, 0x08, 0x00);          /* TXREQ off */
    mcp_bit_modify(REG_CANINTF, 0x04, 0x00);           /* TX0IF off */

    /* 0x7DF standard ID; ISO-TP single frame: len=2, mode=01, pid=00 */
    uint8_t frame[13] = {
        (uint8_t)(OBD_REQ_ID >> 3),          /* SIDH */
        (uint8_t)((OBD_REQ_ID & 0x07) << 5), /* SIDL */
        0x00, 0x00,                          /* EID8/EID0 unused */
        0x08,                                /* DLC = 8 */
        0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    mcp_write_regs(REG_TXB0SIDH, frame, sizeof(frame));
    ESP_ERROR_CHECK(mcp_cmd(MCP_CMD_RTS_TXB0));
}

static bool read_rx_buffer(int which, uint16_t *id, uint8_t *dlc, uint8_t *data)
{
    uint8_t base = (which == 0) ? REG_RXB0SIDH : REG_RXB1SIDH;
    uint8_t raw[13];
    mcp_read_regs(base, raw, sizeof(raw));
    *id = ((uint16_t)raw[0] << 3) | (raw[1] >> 5);
    *dlc = raw[4] & 0x0F;
    if (*dlc > 8) {
        *dlc = 8;
    }
    for (int i = 0; i < *dlc; i++) {
        data[i] = raw[5 + i];
    }
    mcp_bit_modify(REG_CANINTF, (which == 0) ? INTF_RX0IF : INTF_RX1IF, 0x00);
    return true;
}

static void log_frame(uint16_t id, uint8_t dlc, const uint8_t *data)
{
    char hex[3 * 8 + 1] = {0};
    for (int i = 0; i < dlc; i++) {
        snprintf(hex + i * 3, 4, "%02X ", data[i]);
    }
    bool is_obd = (id >= OBD_RESP_LO && id <= OBD_RESP_HI);
    ESP_LOGI(TAG, "RX id=0x%03X dlc=%u  [%s] %s", id, dlc, hex,
             is_obd ? "<-- OBD ECU RESPONSE" : "");

    if (is_obd && dlc >= 3 && data[1] == 0x41 && data[2] == 0x00) {
        ESP_LOGI(TAG, "*** Mode 01 PID 00 reply: protocol confirmed, ECU 0x%03X ***", id);
    }
}

static void probe_once(void)
{
    send_obd_0100();

    int responses = 0;
    int64_t deadline = esp_timer_get_time() + 1000000;  /* 1 s window */
    while (esp_timer_get_time() < deadline) {
        uint8_t intf = mcp_read_reg(REG_CANINTF);
        if (intf & (INTF_RX0IF | INTF_RX1IF)) {
            uint16_t id;
            uint8_t dlc, data[8];
            if (intf & INTF_RX0IF) {
                read_rx_buffer(0, &id, &dlc, data);
                log_frame(id, dlc, data);
                responses++;
            }
            if (intf & INTF_RX1IF) {
                read_rx_buffer(1, &id, &dlc, data);
                log_frame(id, dlc, data);
                responses++;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    uint8_t txctrl = mcp_read_reg(REG_TXB0CTRL);
    uint8_t tec = mcp_read_reg(REG_TEC);
    uint8_t rec = mcp_read_reg(REG_REC);
    uint8_t eflg = mcp_read_reg(REG_EFLG);

    if (responses == 0) {
        ESP_LOGW(TAG, "no response. TXB0CTRL=0x%02X TEC=%u REC=%u EFLG=0x%02X",
                 txctrl, tec, rec, eflg);
        if (txctrl & 0x08) {
            ESP_LOGW(TAG, "TX never completed (no ACK): wrong bitrate, bus not "
                          "connected, or ignition off");
        } else if (tec == 0 && eflg == 0) {
            ESP_LOGW(TAG, "TX acknowledged but no OBD reply: bus alive, ECU "
                          "may need 29-bit IDs or is gateway-filtered");
        }
        /* Abort so retries don't spam the bus between probes. */
        mcp_bit_modify(REG_TXB0CTRL, 0x08, 0x00);
    } else {
        ESP_LOGI(TAG, "probe done: %d frame(s), TEC=%u REC=%u EFLG=0x%02X",
                 responses, tec, rec, eflg);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "MCP2515 OBD probe  SCK=%d MOSI=%d MISO=%d CS=%d INT=%d",
             PIN_SCK, PIN_MOSI, PIN_MISO, PIN_CS, PIN_INT);

    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << PIN_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&int_cfg));

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    while (!smoke_test()) {
        ESP_LOGE(TAG, "SPI smoke test failing; fix wiring, retrying in 3 s");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    while (!can_configure_500k()) {
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    while (true) {
        probe_once();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
