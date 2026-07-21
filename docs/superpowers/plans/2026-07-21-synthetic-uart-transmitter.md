# Synthetic UART Transmitter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and flash a standalone ESP32-C6 firmware that emits one realistic fleet-telemetry JSON line per second over UART1 and logs fleet-device RX traffic over USB Serial/JTAG.

**Architecture:** A separate ESP-IDF project under `examples/synthetic_uart/` preserves the working root ELM327 application. A pure deterministic model generates repeatable values and raw OBD strings; `app_main.c` serializes the model with cJSON, transmits it over UART1, and runs an independent UART RX logging task.

**Tech Stack:** ESP-IDF 5.2, ESP32-C6, FreeRTOS, ESP-IDF UART driver, cJSON, C host test compiled with the system C compiler.

## Global Constraints

- UART1 TX is GPIO 17 and UART1 RX is GPIO 16.
- UART format is 9600 baud, 8 data bits, no parity, 1 stop bit.
- The endpoints share ground and both use 3.3 V TTL UART levels, not RS-232.
- Transmit one compact newline-delimited JSON object every 1000 ms.
- Keep this as a standalone project; do not modify the root ELM327 application.
- UART RX has no ACK requirement; received bytes are logged with `[fleet-rx]`.

---

## File Structure

- `examples/synthetic_uart/CMakeLists.txt` — standalone ESP-IDF project entry point.
- `examples/synthetic_uart/sdkconfig.defaults` — ESP32-C6 target and USB Serial/JTAG console defaults.
- `examples/synthetic_uart/main/CMakeLists.txt` — app component sources and dependencies.
- `examples/synthetic_uart/main/synthetic_data.h` — transport-independent synthetic snapshot interface.
- `examples/synthetic_uart/main/synthetic_data.c` — deterministic telemetry values and raw OBD encoding.
- `examples/synthetic_uart/main/app_main.c` — UART initialization, cJSON serialization, TX and RX tasks.
- `examples/synthetic_uart/tests/CMakeLists.txt` — host-test build.
- `examples/synthetic_uart/tests/test_synthetic_data.c` — deterministic value/range/raw-field tests.
- `examples/synthetic_uart/README.md` — wiring, build, flash, framing, and validation instructions.

---

### Task 1: Deterministic synthetic telemetry model

**Files:**
- Create: `examples/synthetic_uart/main/synthetic_data.h`
- Create: `examples/synthetic_uart/main/synthetic_data.c`
- Create: `examples/synthetic_uart/tests/CMakeLists.txt`
- Create: `examples/synthetic_uart/tests/test_synthetic_data.c`

**Interfaces:**
- Produces: `void synthetic_snapshot_generate(uint32_t sequence, uint64_t uptime_ms, synthetic_snapshot_t *out)`
- Produces: `synthetic_snapshot_t` containing sequence, timestamps, numeric values, and consistent raw response strings.
- Consumes: standard C library only; no ESP-IDF headers, allowing host tests.

- [ ] **Step 1: Write the model header**

Create `examples/synthetic_uart/main/synthetic_data.h`:

```c
#pragma once

#include <stdint.h>

typedef struct {
    uint32_t sequence;
    uint64_t ts_ms;
    uint64_t uptime_s;
    double rpm;
    uint8_t speed_kmh;
    int16_t coolant_c;
    double throttle_pct;
    double voltage_v;
    char rpm_raw[11];
    char speed_raw[7];
    char coolant_raw[7];
    char throttle_raw[7];
    char voltage_raw[12];
} synthetic_snapshot_t;

void synthetic_snapshot_generate(uint32_t sequence, uint64_t uptime_ms,
                                 synthetic_snapshot_t *out);
```

- [ ] **Step 2: Write the failing host test**

Create `examples/synthetic_uart/tests/test_synthetic_data.c`:

```c
#include "synthetic_data.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_first_snapshot(void)
{
    synthetic_snapshot_t s;
    synthetic_snapshot_generate(0, 1000, &s);

    assert(s.sequence == 0);
    assert(s.ts_ms == 1000);
    assert(s.uptime_s == 1);
    assert(s.rpm >= 750.0 && s.rpm <= 2500.0);
    assert(s.speed_kmh <= 80);
    assert(s.coolant_c >= 85 && s.coolant_c <= 95);
    assert(s.throttle_pct >= 10.0 && s.throttle_pct <= 60.0);
    assert(s.voltage_v >= 13.5 && s.voltage_v <= 14.2);
    assert(strncmp(s.rpm_raw, "410C", 4) == 0);
    assert(strncmp(s.speed_raw, "410D", 4) == 0);
    assert(strncmp(s.coolant_raw, "4105", 4) == 0);
    assert(strncmp(s.throttle_raw, "4111", 4) == 0);
}

static void test_repeatable_and_changing(void)
{
    synthetic_snapshot_t a;
    synthetic_snapshot_t b;
    synthetic_snapshot_t again;
    synthetic_snapshot_generate(5, 6000, &a);
    synthetic_snapshot_generate(6, 7000, &b);
    synthetic_snapshot_generate(5, 6000, &again);

    assert(memcmp(&a, &again, sizeof(a)) == 0);
    assert(a.speed_kmh != b.speed_kmh || fabs(a.rpm - b.rpm) > 0.01);
}

int main(void)
{
    test_first_snapshot();
    test_repeatable_and_changing();
    puts("synthetic_data tests passed");
    return 0;
}
```

Create `examples/synthetic_uart/tests/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(synthetic_data_tests C)

set(CMAKE_C_STANDARD 11)
add_executable(test_synthetic_data
    test_synthetic_data.c
    ../main/synthetic_data.c
)
target_include_directories(test_synthetic_data PRIVATE ../main)
target_link_libraries(test_synthetic_data m)
```

- [ ] **Step 3: Run the test to verify it fails**

Run:

```bash
cmake -S examples/synthetic_uart/tests -B examples/synthetic_uart/tests/build
cmake --build examples/synthetic_uart/tests/build
```

Expected: compile or link failure because `synthetic_snapshot_generate` is not implemented.

- [ ] **Step 4: Implement the deterministic model**

Create `examples/synthetic_uart/main/synthetic_data.c`:

```c
#include "synthetic_data.h"

#include <stdio.h>
#include <string.h>

void synthetic_snapshot_generate(uint32_t sequence, uint64_t uptime_ms,
                                 synthetic_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->sequence = sequence;
    out->ts_ms = uptime_ms;
    out->uptime_s = uptime_ms / 1000U;

    uint32_t phase = sequence % 160U;
    uint32_t ramp = phase <= 80U ? phase : 160U - phase;

    out->speed_kmh = (uint8_t)ramp;
    out->rpm = 780.0 + (double)ramp * 20.0;
    out->coolant_c = (int16_t)(85 + (sequence % 11U));
    out->throttle_pct = 10.0 + (double)(sequence % 51U);
    out->voltage_v = 13.5 + (double)(sequence % 8U) / 10.0;

    uint16_t rpm_x4 = (uint16_t)(out->rpm * 4.0);
    uint8_t coolant_a = (uint8_t)(out->coolant_c + 40);
    uint8_t throttle_a = (uint8_t)((out->throttle_pct * 255.0 / 100.0) + 0.5);

    snprintf(out->rpm_raw, sizeof(out->rpm_raw), "410C%02X%02X",
             (rpm_x4 >> 8) & 0xFF, rpm_x4 & 0xFF);
    snprintf(out->speed_raw, sizeof(out->speed_raw), "410D%02X", out->speed_kmh);
    snprintf(out->coolant_raw, sizeof(out->coolant_raw), "4105%02X", coolant_a);
    snprintf(out->throttle_raw, sizeof(out->throttle_raw), "4111%02X", throttle_a);
    snprintf(out->voltage_raw, sizeof(out->voltage_raw), "%.1fV", out->voltage_v);
}
```

- [ ] **Step 5: Run host tests**

Run:

```bash
cmake --build examples/synthetic_uart/tests/build
examples/synthetic_uart/tests/build/test_synthetic_data
```

Expected:

```text
synthetic_data tests passed
```

- [ ] **Step 6: Commit the model**

```bash
git add examples/synthetic_uart/main/synthetic_data.h \
        examples/synthetic_uart/main/synthetic_data.c \
        examples/synthetic_uart/tests/CMakeLists.txt \
        examples/synthetic_uart/tests/test_synthetic_data.c
git commit -m "test: add deterministic synthetic fleet telemetry model"
```

---

### Task 2: Standalone UART transmitter firmware

**Files:**
- Create: `examples/synthetic_uart/CMakeLists.txt`
- Create: `examples/synthetic_uart/sdkconfig.defaults`
- Create: `examples/synthetic_uart/main/CMakeLists.txt`
- Create: `examples/synthetic_uart/main/app_main.c`

**Interfaces:**
- Consumes: `synthetic_snapshot_generate(...)` from Task 1.
- Produces: one newline-delimited fleet snapshot per second on UART1 TX GPIO 17.
- Produces: USB console logs tagged `[fleet-tx]` and `[fleet-rx]`.

- [ ] **Step 1: Add the standalone ESP-IDF project files**

Create `examples/synthetic_uart/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(synthetic_uart)
```

Create `examples/synthetic_uart/sdkconfig.defaults`:

```text
CONFIG_IDF_TARGET="esp32c6"
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

Create `examples/synthetic_uart/main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "app_main.c" "synthetic_data.c"
    INCLUDE_DIRS "."
    REQUIRES driver json esp_timer
)
```

- [ ] **Step 2: Implement JSON construction and UART tasks**

Create `examples/synthetic_uart/main/app_main.c`:

```c
#include "synthetic_data.h"

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLEET_UART UART_NUM_1
#define FLEET_TX_GPIO 17
#define FLEET_RX_GPIO 16
#define FLEET_BAUD 9600
#define RX_BUF_SIZE 512

static const char *TAG = "synthetic_uart";
static uint32_t s_cmds_ok;
static uint32_t s_cmds_fail;

static cJSON *add_sample(cJSON *samples, const char *key, const char *cmd,
                         double value, const char *unit, const char *raw)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return NULL;
    }
    cJSON_AddStringToObject(item, "k", key);
    cJSON_AddStringToObject(item, "cmd", cmd);
    cJSON_AddNumberToObject(item, "v", value);
    cJSON_AddStringToObject(item, "u", unit);
    cJSON_AddStringToObject(item, "raw", raw);
    cJSON_AddBoolToObject(item, "ok", true);
    cJSON_AddNumberToObject(item, "age_ms", 0);
    cJSON_AddItemToArray(samples, item);
    return item;
}

static char *build_snapshot_json(const synthetic_snapshot_t *s, uint32_t cmds_ok)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *link = cJSON_CreateObject();
    cJSON *metrics = cJSON_CreateObject();
    cJSON *samples = cJSON_CreateArray();
    if (!root || !link || !metrics || !samples) {
        cJSON_Delete(root);
        cJSON_Delete(link);
        cJSON_Delete(metrics);
        cJSON_Delete(samples);
        return NULL;
    }

    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "node_id", "esp32c6-01");
    cJSON_AddStringToObject(root, "vehicle_id", "fleet-demo-001");
    cJSON_AddStringToObject(root, "ble_peer", "46:FC:0D:32:1E:66");
    cJSON_AddStringToObject(root, "adapter", "MODAXE OBDII");
    cJSON_AddStringToObject(root, "profile", "can_11_500");
    cJSON_AddStringToObject(root, "protocol", "ISO15765-4 CAN11/500");
    cJSON_AddNumberToObject(root, "seq", s->sequence);
    cJSON_AddNumberToObject(root, "ts_ms", (double)s->ts_ms);
    cJSON_AddNumberToObject(root, "uptime_s", (double)s->uptime_s);

    cJSON_AddBoolToObject(link, "ble_connected", true);
    cJSON_AddBoolToObject(link, "elm_ready", true);
    cJSON_AddStringToObject(link, "poller", "on");
    cJSON_AddItemToObject(root, "link", link);

    cJSON_AddNumberToObject(metrics, "cmds_ok", cmds_ok);
    cJSON_AddNumberToObject(metrics, "cmds_fail", s_cmds_fail);
    cJSON_AddNumberToObject(metrics, "ble_reconnects", 0);
    cJSON_AddNumberToObject(metrics, "blocked_cmds", 0);
    cJSON_AddNumberToObject(metrics, "telemetry_drops", 0);
    cJSON_AddItemToObject(root, "metrics", metrics);

    add_sample(samples, "rpm", "010C", s->rpm, "rpm", s->rpm_raw);
    add_sample(samples, "speed", "010D", s->speed_kmh, "km/h", s->speed_raw);
    add_sample(samples, "coolant_c", "0105", s->coolant_c, "C", s->coolant_raw);
    add_sample(samples, "throttle_pct", "0111", s->throttle_pct, "%", s->throttle_raw);
    add_sample(samples, "voltage", "ATRV", s->voltage_v, "V", s->voltage_raw);
    cJSON_AddItemToObject(root, "samples", samples);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

static void fleet_tx_task(void *arg)
{
    (void)arg;
    uint32_t sequence = 0;
    TickType_t next_wake = xTaskGetTickCount();
    for (;;) {
        synthetic_snapshot_t snapshot;
        synthetic_snapshot_generate(sequence, (uint64_t)(esp_timer_get_time() / 1000),
                                    &snapshot);
        char *json = build_snapshot_json(&snapshot, s_cmds_ok + 1);
        if (!json) {
            ++s_cmds_fail;
            ESP_LOGE(TAG, "JSON allocation failed");
        } else {
            size_t len = strlen(json);
            int written = uart_write_bytes(FLEET_UART, json, len);
            int newline_written = uart_write_bytes(FLEET_UART, "\n", 1);
            if (written == (int)len && newline_written == 1) {
                ++s_cmds_ok;
                printf("[fleet-tx] %s\n", json);
            } else {
                ++s_cmds_fail;
                ESP_LOGE(TAG, "short UART write: %d/%u", written, (unsigned)len);
            }
            cJSON_free(json);
        }
        ++sequence;
        xTaskDelayUntil(&next_wake, pdMS_TO_TICKS(1000));
    }
}

static void fleet_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[RX_BUF_SIZE + 1];
    for (;;) {
        int len = uart_read_bytes(FLEET_UART, buf, RX_BUF_SIZE,
                                  pdMS_TO_TICKS(200));
        if (len > 0) {
            buf[len] = '\0';
            printf("[fleet-rx] %.*s\n", len, (char *)buf);
        }
    }
}

void app_main(void)
{
    const uart_config_t config = {
        .baud_rate = FLEET_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* TX buffering prevents a ~750-byte frame at 9600 baud from extending
     * the one-second producer cadence. */
    ESP_ERROR_CHECK(uart_driver_install(FLEET_UART, RX_BUF_SIZE * 2, 2048,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(FLEET_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(FLEET_UART, FLEET_TX_GPIO, FLEET_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART1 ready: TX=GPIO%d RX=GPIO%d 9600 8N1",
             FLEET_TX_GPIO, FLEET_RX_GPIO);
    ESP_ERROR_CHECK(xTaskCreate(fleet_tx_task, "fleet_tx", 6144, NULL, 5, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(fleet_rx_task, "fleet_rx", 3072, NULL, 4, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
}
```

- [ ] **Step 3: Build the standalone firmware**

Run:

```bash
source ~/esp/esp-idf/export.sh
idf.py -C examples/synthetic_uart set-target esp32c6
idf.py -C examples/synthetic_uart build
```

Expected: build completes and prints the generated binary path and flash command without errors.

- [ ] **Step 4: Check edited-file diagnostics**

Run IDE diagnostics for:

- `examples/synthetic_uart/main/app_main.c`
- `examples/synthetic_uart/main/synthetic_data.c`
- `examples/synthetic_uart/main/synthetic_data.h`

Expected: no new errors.

- [ ] **Step 5: Commit the standalone firmware**

```bash
git add examples/synthetic_uart/CMakeLists.txt \
        examples/synthetic_uart/sdkconfig.defaults \
        examples/synthetic_uart/main/CMakeLists.txt \
        examples/synthetic_uart/main/app_main.c
git commit -m "feat: add standalone synthetic UART transmitter"
```

---

### Task 3: Documentation and hardware verification

**Files:**
- Create: `examples/synthetic_uart/README.md`

**Interfaces:**
- Consumes: standalone firmware from Task 2.
- Produces: reproducible wiring, flash, framing, and fleet RX verification instructions.

- [ ] **Step 1: Write the standalone project README**

Create `examples/synthetic_uart/README.md`:

```markdown
# Synthetic UART Fleet Transmitter

Standalone ESP32-C6 test firmware. It sends one compact fleet telemetry JSON
object per second over UART1.

## Wiring

| ESP32-C6 Super Mini | Fleet device |
|---|---|
| GPIO 17 (TX) | RX |
| GPIO 16 (RX) | TX |
| GND | GND |

Both endpoints must use 3.3 V TTL UART levels. Do not connect directly to an
RS-232 voltage interface.

Serial format: 9600 baud, 8 data bits, no parity, 1 stop bit.

## Build and flash

```bash
source ~/esp/esp-idf/export.sh
idf.py -C examples/synthetic_uart set-target esp32c6
idf.py -C examples/synthetic_uart build
idf.py -C examples/synthetic_uart -p /dev/cu.usbmodemXXXX flash monitor
```

Exit the monitor with `Ctrl+]`.

## Framing

Each UART frame is one compact JSON object followed by LF (`\n`). The fleet
device should buffer bytes until LF, then parse the complete line as JSON.

USB console output uses:

```text
[fleet-tx] {"v":1,...}
[fleet-rx] ACK
```

The receiver does not require ACKs and does not retransmit frames.
```

- [ ] **Step 2: Run all pre-flash verification**

Run:

```bash
cmake --build examples/synthetic_uart/tests/build
examples/synthetic_uart/tests/build/test_synthetic_data
idf.py -C examples/synthetic_uart build
```

Expected:

```text
synthetic_data tests passed
```

and an ESP-IDF successful-build message.

- [ ] **Step 3: Identify the connected ESP32-C6 port**

Run:

```bash
ls /dev/cu.usbmodem*
```

Expected: one connected USB Serial/JTAG device such as `/dev/cu.usbmodem101`. If multiple devices appear, stop and confirm the correct board before flashing.

- [ ] **Step 4: Flash the standalone image**

Run with the discovered port:

```bash
idf.py -C examples/synthetic_uart -p /dev/cu.usbmodem101 flash
```

Expected: flash verification succeeds and the ESP32-C6 resets.

- [ ] **Step 5: Monitor and validate three JSON frames**

Open the USB console at 115200 baud, capture three lines beginning `[fleet-tx]`, strip the prefix, and parse each remainder with a JSON parser.

Expected for every frame:

- valid JSON
- `v == 1`
- `seq` increments by one
- five entries in `samples`
- TX cadence is approximately one second
- RPM, speed, coolant, throttle, and voltage remain in the documented ranges

If GPIO 17 and GPIO 16 are connected as a loopback, also expect the transmitted JSON bytes to appear under `[fleet-rx]`.

- [ ] **Step 6: Commit documentation**

```bash
git add examples/synthetic_uart/README.md
git commit -m "docs: add synthetic UART wiring and validation guide"
```

