/*
 * MCP2515 bench diagnostic — isolates SPI path from main fleet firmware.
 *
 * Same GPIO map as production: SCK=21 MOSI=22 MISO=23 CS=20 INT=14.
 * Runs three probes every 3 s:
 *   1) production-style bit-bang (no per-edge delay)
 *   2) slow bit-bang (5 us half-period)
 *   3) hardware SPI2 @ 1 MHz (same pins as examples/mcp2515_smoke)
 *
 * Flash: idf.py -C examples/mcp2515_diag set-target esp32c6 build flash monitor
 * Restore main app: idf.py -p PORT flash   (from repo root)
 */

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mcp_diag";

#define PIN_SCK   21
#define PIN_MOSI  22
#define PIN_MISO  23
#define PIN_CS    20
#define PIN_INT   14

#define MCP_CMD_RESET 0xC0
#define MCP_CMD_READ  0x03
#define MCP_CMD_WRITE 0x02

#define REG_CANSTAT 0x0E
#define REG_CANCTRL 0x0F
#define REG_CNF1    0x2A

typedef struct {
    int sck;
    int mosi;
    int miso;
    int cs;
    uint32_t half_period_us;
    bool hw_spi;
    spi_device_handle_t hw;
} bus_t;

static void cs_l(bus_t *b) { gpio_set_level(b->cs, 0); }
static void cs_h(bus_t *b) { gpio_set_level(b->cs, 1); }

static void half_delay(bus_t *b)
{
    if (b->half_period_us > 0) {
        esp_rom_delay_us(b->half_period_us);
    }
}

static uint8_t soft_xfer_byte(bus_t *b, uint8_t out)
{
    uint8_t in = 0;
    for (int i = 0; i < 8; i++) {
        gpio_set_level(b->mosi, (out & 0x80) ? 1 : 0);
        out <<= 1;
        half_delay(b);
        gpio_set_level(b->sck, 1);
        half_delay(b);
        in = (uint8_t)((in << 1) | (gpio_get_level(b->miso) & 1));
        gpio_set_level(b->sck, 0);
    }
    return in;
}

static void soft_xfer(bus_t *b, const uint8_t *tx, uint8_t *rx, size_t n)
{
    cs_l(b);
    for (size_t i = 0; i < n; i++) {
        uint8_t v = soft_xfer_byte(b, tx ? tx[i] : 0xFF);
        if (rx) {
            rx[i] = v;
        }
    }
    cs_h(b);
}

static esp_err_t hw_cmd(bus_t *b, uint8_t cmd)
{
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    return spi_device_transmit(b->hw, &t);
}

static uint8_t hw_read_reg(bus_t *b, uint8_t reg)
{
    uint8_t tx[3] = {MCP_CMD_READ, reg, 0x00};
    uint8_t rx[3] = {0};
    spi_transaction_t t = {.length = 24, .tx_buffer = tx, .rx_buffer = rx};
    ESP_ERROR_CHECK(spi_device_transmit(b->hw, &t));
    return rx[2];
}

static void hw_write_reg(bus_t *b, uint8_t reg, uint8_t val)
{
    uint8_t tx[3] = {MCP_CMD_WRITE, reg, val};
    spi_transaction_t t = {.length = 24, .tx_buffer = tx};
    ESP_ERROR_CHECK(spi_device_transmit(b->hw, &t));
}

static uint8_t soft_read_reg(bus_t *b, uint8_t reg)
{
    uint8_t tx[3] = {MCP_CMD_READ, reg, 0x00};
    uint8_t rx[3] = {0};
    soft_xfer(b, tx, rx, sizeof(tx));
    return rx[2];
}

static void soft_write_reg(bus_t *b, uint8_t reg, uint8_t val)
{
    uint8_t tx[3] = {MCP_CMD_WRITE, reg, val};
    soft_xfer(b, tx, NULL, sizeof(tx));
}

static void soft_reset(bus_t *b)
{
    uint8_t cmd = MCP_CMD_RESET;
    soft_xfer(b, &cmd, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void log_probe(const char *label, uint8_t canstat, uint8_t canctrl,
                      uint8_t cnf1_before, uint8_t cnf1_after, int miso_idle)
{
    bool cfg_ok = ((canstat & 0xE0) == 0x80);
    bool rb_ok = (cnf1_after == 0x55);
    ESP_LOGI(TAG, "%s: miso_idle=%d CANSTAT=0x%02X CANCTRL=0x%02X "
                  "CNF1 before=0x%02X after=0x%02X -> %s",
             label, miso_idle, canstat, canctrl, cnf1_before, cnf1_after,
             (cfg_ok && rb_ok) ? "PASS" : "FAIL");
    if (!cfg_ok) {
        ESP_LOGW(TAG, "%s: expected CANSTAT[7:5]=Config(0x80), got 0x%02X",
                 label, canstat & 0xE0);
    }
    if (!rb_ok) {
        ESP_LOGW(TAG, "%s: CNF1 write/read mismatch (SPI MISO/MOSI/CS/SCK path)",
                 label);
    }
}

static void probe_soft(bus_t *b, const char *label)
{
    int miso_idle = gpio_get_level(b->miso);
    soft_reset(b);
    uint8_t canstat = soft_read_reg(b, REG_CANSTAT);
    uint8_t canctrl = soft_read_reg(b, REG_CANCTRL);
    uint8_t cnf1_before = soft_read_reg(b, REG_CNF1);
    soft_write_reg(b, REG_CNF1, 0x55);
    uint8_t cnf1_after = soft_read_reg(b, REG_CNF1);
    log_probe(label, canstat, canctrl, cnf1_before, cnf1_after, miso_idle);
}

static void probe_hw(bus_t *b, const char *label)
{
    int miso_idle = gpio_get_level(b->miso);
    ESP_ERROR_CHECK(hw_cmd(b, MCP_CMD_RESET));
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t canstat = hw_read_reg(b, REG_CANSTAT);
    uint8_t canctrl = hw_read_reg(b, REG_CANCTRL);
    uint8_t cnf1_before = hw_read_reg(b, REG_CNF1);
    hw_write_reg(b, REG_CNF1, 0x55);
    uint8_t cnf1_after = hw_read_reg(b, REG_CNF1);
    log_probe(label, canstat, canctrl, cnf1_before, cnf1_after, miso_idle);
}

static void gpio_setup(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_SCK) | (1ULL << PIN_MOSI) | (1ULL << PIN_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_MISO) | (1ULL << PIN_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    ESP_ERROR_CHECK(gpio_config(&in));
    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_SCK, 0);
    gpio_set_level(PIN_MOSI, 0);
}

static esp_err_t hw_bus_init(bus_t *b)
{
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 4,
    };
    return spi_bus_add_device(SPI2_HOST, &dev, &b->hw);
}

static void pulse_outputs(void)
{
    /* 2 Hz pulses so a meter/scope can confirm firmware is driving the GPIOs. */
    ESP_LOGI(TAG, "pulsing CS=20 SCK=21 MOSI=22 for 2 s (MISO=23 / INT=14 are inputs)");
    for (int i = 0; i < 4; i++) {
        gpio_set_level(PIN_CS, 0);
        gpio_set_level(PIN_SCK, 1);
        gpio_set_level(PIN_MOSI, 1);
        vTaskDelay(pdMS_TO_TICKS(250));
        gpio_set_level(PIN_CS, 1);
        gpio_set_level(PIN_SCK, 0);
        gpio_set_level(PIN_MOSI, 0);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "MCP2515 test map (user-validated PCB):");
    ESP_LOGI(TAG, "  CS  B1=A1 GPIO%d", PIN_CS);
    ESP_LOGI(TAG, "  SO  B2=A2 GPIO%d  (MISO)", PIN_MISO);
    ESP_LOGI(TAG, "  SI  B3=A3 GPIO%d  (MOSI)", PIN_MOSI);
    ESP_LOGI(TAG, "  SCK B4=A4 GPIO%d", PIN_SCK);
    ESP_LOGI(TAG, "  INT B5=A5 GPIO%d", PIN_INT);

    gpio_setup();
    pulse_outputs();

    bus_t fast = {
        .sck = PIN_SCK, .mosi = PIN_MOSI, .miso = PIN_MISO, .cs = PIN_CS,
        .half_period_us = 0, .hw_spi = false,
    };
    bus_t slow = fast;
    slow.half_period_us = 5;

    /* One-shot: SI/SO swapped (GPIO22=MISO, GPIO23=MOSI). If this PASSes,
     * traces are live but SI/SO are crossed at the MCP header. */
    {
        gpio_config_t out = {
            .pin_bit_mask = (1ULL << PIN_SCK) | (1ULL << PIN_MISO) | (1ULL << PIN_CS),
            .mode = GPIO_MODE_OUTPUT,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config_t in = {
            .pin_bit_mask = (1ULL << PIN_MOSI) | (1ULL << PIN_INT),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&out);
        gpio_config(&in);
        gpio_set_level(PIN_CS, 1);
        gpio_set_level(PIN_SCK, 0);
        bus_t swap = {
            .sck = PIN_SCK, .mosi = PIN_MISO, .miso = PIN_MOSI, .cs = PIN_CS,
            .half_period_us = 5, .hw_spi = false,
        };
        probe_soft(&swap, "SI/SO swapped (22=MISO 23=MOSI)");
        gpio_setup();
    }

    bus_t hw = {
        .sck = PIN_SCK, .mosi = PIN_MOSI, .miso = PIN_MISO, .cs = PIN_CS,
        .hw_spi = true,
    };
    bool hw_ok = (hw_bus_init(&hw) == ESP_OK);

    while (true) {
        ESP_LOGI(TAG, "--- probe cycle ---");
        probe_soft(&fast, "soft-spi fast (production-like)");
        probe_soft(&slow, "soft-spi slow (5us half-period)");
        if (hw_ok) {
            probe_hw(&hw, "hw-spi2 1MHz");
        } else {
            ESP_LOGW(TAG, "hw-spi2: bus init failed");
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
