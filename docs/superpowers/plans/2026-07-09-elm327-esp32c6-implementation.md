# ELM327 ESP32-C6 BLE Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build production-ready ESP-IDF firmware that connects ESP32-C6 Mini to a BLE ELM327, polls OBD data via configurable profiles behind a read-only safety gate, and exposes data over serial + SoftAP HTTP transports.

**Architecture:** Layered ESP-IDF components: `ble_elm` → `elm_transport` → `elm327_client` → `cmd_policy` → `obd_poller`/`obd_codec` → `telemetry_bus` → replaceable transports (`transport_serial`, `transport_http`). OBD logic never depends on HTTP/Zigbee/UART fleet.

**Tech Stack:** ESP-IDF (ESP32-C6), NimBLE, FreeRTOS, esp_http_server, SoftAP, NVS, cJSON, host GCC unit tests for pure-C modules.

**Spec:** `docs/superpowers/specs/2026-07-09-elm327-esp32c6-design.md`

**ESP-IDF path (this machine):** `~/esp/esp-idf` — source `export.sh` before `idf.py` commands.

---

## File structure (create)

```
ELM327_esp/
  CMakeLists.txt
  sdkconfig.defaults
  partitions.csv
  main/
    CMakeLists.txt
    Kconfig.projbuild
    app_main.c
  components/
    cmd_policy/          # allowlist gate (pure C + thin ESP wrapper)
    obd_codec/           # PID/DTC/VIN decode (pure C)
    elm_transport/       # transport interface header + helpers
    telemetry_bus/       # pub/sub
    sys_runtime/         # WDT, metrics, logging helpers, OTA stub
    profile_store/       # NVS JSON profiles + BLE bond + safety flags
    ble_elm/             # NimBLE central + NUS
    elm327_client/       # AT session over elm_transport
    obd_poller/          # profile scheduler + raw cmd queue
    transport_serial/    # console CLI
    transport_http/      # SoftAP + REST + embedded HTML
  tests/host/
    CMakeLists.txt
    test_cmd_policy.c
    test_obd_codec.c
  docs/superpowers/...
```

---

### Task 1: Git init + ESP-IDF project skeleton

**Files:**
- Create: `CMakeLists.txt`
- Create: `sdkconfig.defaults`
- Create: `partitions.csv`
- Create: `main/CMakeLists.txt`
- Create: `main/Kconfig.projbuild`
- Create: `main/app_main.c`
- Create: `.gitignore`

- [ ] **Step 1: Initialize git repository**

```bash
cd /Users/nc23896-mithun/Documents/office/nc_rnd/fleet/ELM327_esp
git init
```

- [ ] **Step 2: Create `.gitignore`**

```gitignore
build/
sdkconfig
sdkconfig.old
dependencies.lock
managed_components/
.vscode/
.idea/
*.pyc
__pycache__/
.DS_Store
tests/host/build/
```

- [ ] **Step 3: Create root `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(elm327_esp32c6)
```

- [ ] **Step 4: Create `sdkconfig.defaults`**

```
CONFIG_IDF_TARGET="esp32c6"
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

- [ ] **Step 5: Create `partitions.csv` (factory + ota stubs + nvs)**

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
otadata,  data, ota,     0xf000,  0x2000,
phy_init, data, phy,     0x11000, 0x1000,
factory,  app,  factory, 0x20000, 0x1E0000,
ota_0,    app,  ota_0,   0x200000,0x1E0000,
ota_1,    app,  ota_1,   0x3E0000,0x1E0000,
```

- [ ] **Step 6: Create `main/CMakeLists.txt` and stub `app_main.c`**

`main/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash)
```

`main/app_main.c`:
```c
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "ELM327 ESP32-C6 bridge boot");
}
```

`main/Kconfig.projbuild`:
```
menu "ELM327 Bridge"

    config ELM_SOFTAP_SSID
        string "SoftAP SSID"
        default "ELM327-C6"

    config ELM_SOFTAP_PASS
        string "SoftAP password"
        default "elm327c6"
        help
            Minimum 8 characters.

    config ELM_CMD_TIMEOUT_MS
        int "ELM command timeout (ms)"
        default 3000
        range 500 30000

    config ELM_BLE_SCAN_MS
        int "BLE scan duration (ms)"
        default 5000
        range 1000 30000

endmenu
```

- [ ] **Step 7: Verify project configures**

```bash
source ~/esp/esp-idf/export.sh
cd /Users/nc23896-mithun/Documents/office/nc_rnd/fleet/ELM327_esp
idf.py set-target esp32c6
idf.py build
```

Expected: build succeeds (empty app).

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt sdkconfig.defaults partitions.csv main/ .gitignore docs/
git commit -m "$(cat <<'EOF'
chore: scaffold ESP-IDF project for ESP32-C6 ELM327 bridge

EOF
)"
```

---

### Task 2: `cmd_policy` — allowlist safety gate (TDD)

**Files:**
- Create: `components/cmd_policy/include/cmd_policy.h`
- Create: `components/cmd_policy/cmd_policy.c`
- Create: `components/cmd_policy/CMakeLists.txt`
- Create: `tests/host/CMakeLists.txt`
- Create: `tests/host/test_cmd_policy.c`

- [ ] **Step 1: Write public header**

`components/cmd_policy/include/cmd_policy.h`:
```c
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CMD_POLICY_ALLOW = 0,
    CMD_POLICY_DENY_MODE04 = 1,
    CMD_POLICY_DENY_MODE08 = 2,
    CMD_POLICY_DENY_UNKNOWN = 3,
    CMD_POLICY_DENY_UNSAFE_LOCKED = 4,
} cmd_policy_result_t;

typedef struct {
    bool allow_unsafe; /* NVS flag; still never allows mode 08 in v1 */
} cmd_policy_config_t;

void cmd_policy_normalize(const char *in, char *out, size_t out_len);
cmd_policy_result_t cmd_policy_check(const char *cmd, const cmd_policy_config_t *cfg);
bool cmd_policy_is_allowed(const char *cmd, const cmd_policy_config_t *cfg);
const char *cmd_policy_result_str(cmd_policy_result_t r);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write failing host tests**

`tests/host/test_cmd_policy.c`:
```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cmd_policy.h"

static void expect_allow(const char *cmd)
{
    cmd_policy_config_t cfg = { .allow_unsafe = false };
    assert(cmd_policy_is_allowed(cmd, &cfg));
}

static void expect_deny(const char *cmd, cmd_policy_result_t why)
{
    cmd_policy_config_t cfg = { .allow_unsafe = false };
    assert(cmd_policy_check(cmd, &cfg) == why);
}

int main(void)
{
    char norm[32];
    cmd_policy_normalize(" 01 0c\r", norm, sizeof(norm));
    assert(strcmp(norm, "010C") == 0);

    expect_allow("ATZ");
    expect_allow("ATE0");
    expect_allow("010C");
    expect_allow("03");
    expect_allow("0902");
    expect_allow("ATRV");

    expect_deny("04", CMD_POLICY_DENY_MODE04);
    expect_deny("08", CMD_POLICY_DENY_MODE08);
    expect_deny("081f", CMD_POLICY_DENY_MODE08);
    expect_deny("ZZZZ", CMD_POLICY_DENY_UNKNOWN);
    expect_deny("22F190", CMD_POLICY_DENY_UNKNOWN);

    /* unsafe flag still blocks mode 08 */
    cmd_policy_config_t unsafe = { .allow_unsafe = true };
    assert(cmd_policy_check("04", &unsafe) == CMD_POLICY_ALLOW);
    assert(cmd_policy_check("08", &unsafe) == CMD_POLICY_DENY_MODE08);

    printf("test_cmd_policy: PASS\n");
    return 0;
}
```

`tests/host/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
project(elm_host_tests C)
set(CMAKE_C_STANDARD 11)

add_executable(test_cmd_policy
    test_cmd_policy.c
    ${CMAKE_SOURCE_DIR}/../../components/cmd_policy/cmd_policy.c
)
target_include_directories(test_cmd_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/../../components/cmd_policy/include
)

enable_testing()
add_test(NAME test_cmd_policy COMMAND test_cmd_policy)
```

- [ ] **Step 3: Run tests — expect link/compile failure (no implementation)**

```bash
mkdir -p tests/host/build && cd tests/host/build
cmake .. && cmake --build . --target test_cmd_policy
```

Expected: FAIL (missing `cmd_policy.c` symbols or empty stub).

- [ ] **Step 4: Implement `cmd_policy.c`**

```c
#include "cmd_policy.h"
#include <ctype.h>
#include <string.h>

static const char *k_at_allow[] = {
    "ATZ", "ATD", "ATWS", "ATE0", "ATE1", "ATL0", "ATL1",
    "ATS0", "ATS1", "ATH0", "ATH1",
    "ATSP0", "ATSP1", "ATSP2", "ATSP3", "ATSP4",
    "ATSP5", "ATSP6", "ATSP7", "ATSP8", "ATSP9",
    "ATDP", "ATDPN", "ATRV", "ATI", "AT@1",
};

void cmd_policy_normalize(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    if (!in || !out || out_len == 0) {
        if (out && out_len) out[0] = '\0';
        return;
    }
    for (size_t i = 0; in[i] && j + 1 < out_len; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        out[j++] = (char)toupper(c);
    }
    out[j] = '\0';
}

static bool is_hex_str(const char *s)
{
    if (!s || !*s) return false;
    for (; *s; ++s) {
        if (!isxdigit((unsigned char)*s)) return false;
    }
    return true;
}

static int parse_mode(const char *norm)
{
    /* OBD mode is first two hex digits when length >= 2 and not AT* */
    if (norm[0] == 'A' && norm[1] == 'T') return -1;
    if (!is_hex_str(norm) || strlen(norm) < 2) return -1;
    unsigned mode = 0;
    if (sscanf(norm, "%2x", &mode) != 1) return -1;
    return (int)mode;
}

static bool at_allowed(const char *norm)
{
    for (size_t i = 0; i < sizeof(k_at_allow) / sizeof(k_at_allow[0]); ++i) {
        if (strcmp(norm, k_at_allow[i]) == 0) return true;
    }
    return false;
}

cmd_policy_result_t cmd_policy_check(const char *cmd, const cmd_policy_config_t *cfg)
{
    char norm[64];
    cmd_policy_normalize(cmd, norm, sizeof(norm));
    if (norm[0] == '\0') return CMD_POLICY_DENY_UNKNOWN;

    if (norm[0] == 'A' && norm[1] == 'T') {
        return at_allowed(norm) ? CMD_POLICY_ALLOW : CMD_POLICY_DENY_UNKNOWN;
    }

    int mode = parse_mode(norm);
    if (mode < 0) return CMD_POLICY_DENY_UNKNOWN;

    if (mode == 0x04) {
        if (cfg && cfg->allow_unsafe) return CMD_POLICY_ALLOW;
        return CMD_POLICY_DENY_MODE04;
    }
    if (mode == 0x08) {
        return CMD_POLICY_DENY_MODE08; /* never in v1 */
    }

    switch (mode) {
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x07:
    case 0x0A:
    case 0x09:
        return CMD_POLICY_ALLOW;
    default:
        return CMD_POLICY_DENY_UNKNOWN;
    }
}

bool cmd_policy_is_allowed(const char *cmd, const cmd_policy_config_t *cfg)
{
    return cmd_policy_check(cmd, cfg) == CMD_POLICY_ALLOW;
}

const char *cmd_policy_result_str(cmd_policy_result_t r)
{
    switch (r) {
    case CMD_POLICY_ALLOW: return "allow";
    case CMD_POLICY_DENY_MODE04: return "deny_mode04";
    case CMD_POLICY_DENY_MODE08: return "deny_mode08";
    case CMD_POLICY_DENY_UNKNOWN: return "deny_unknown";
    case CMD_POLICY_DENY_UNSAFE_LOCKED: return "deny_unsafe_locked";
    default: return "deny";
    }
}
```

Add `#include <stdio.h>` for `sscanf`.

`components/cmd_policy/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "cmd_policy.c"
                    INCLUDE_DIRS "include")
```

- [ ] **Step 5: Run host tests — expect PASS**

```bash
cd /Users/nc23896-mithun/Documents/office/nc_rnd/fleet/ELM327_esp/tests/host/build
cmake .. && cmake --build . && ctest --output-on-failure
```

Expected: `test_cmd_policy: PASS` and CTest OK.

- [ ] **Step 6: Commit**

```bash
git add components/cmd_policy tests/host
git commit -m "$(cat <<'EOF'
feat: add cmd_policy allowlist gate with host tests

EOF
)"
```

---

### Task 3: `obd_codec` — PID/DTC/VIN decode (TDD)

**Files:**
- Create: `components/obd_codec/include/obd_codec.h`
- Create: `components/obd_codec/obd_codec.c`
- Create: `components/obd_codec/CMakeLists.txt`
- Create: `tests/host/test_obd_codec.c`
- Modify: `tests/host/CMakeLists.txt`

- [ ] **Step 1: Write header**

`components/obd_codec/include/obd_codec.h`:
```c
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ok;
    const char *name;
    const char *unit;
    double value;
    char raw_hex[48];
} obd_decoded_t;

bool obd_codec_decode_mode01(const char *response, uint8_t pid, obd_decoded_t *out);
int obd_codec_parse_dtcs(const char *response, char out[][6], int max_out);
bool obd_codec_parse_vin(const char *response, char *vin, size_t vin_len);
bool obd_codec_decode_named(const char *decode_key, const char *response, obd_decoded_t *out);

#ifdef __cplusplus
}
#endif
```
- [ ] **Step 2: Write failing tests**

`tests/host/test_obd_codec.c`:
```c
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
```

Append to `tests/host/CMakeLists.txt`:
```cmake
add_executable(test_obd_codec
    test_obd_codec.c
    ${CMAKE_SOURCE_DIR}/../../components/obd_codec/obd_codec.c
)
target_include_directories(test_obd_codec PRIVATE
    ${CMAKE_SOURCE_DIR}/../../components/obd_codec/include
)
target_link_libraries(test_obd_codec m)
add_test(NAME test_obd_codec COMMAND test_obd_codec)
```

- [ ] **Step 3: Run — expect FAIL**

```bash
cd /Users/nc23896-mithun/Documents/office/nc_rnd/fleet/ELM327_esp/tests/host/build
cmake .. && cmake --build . --target test_obd_codec
```

Expected: missing `obd_codec.c` / undefined symbols.

- [ ] **Step 4: Implement `obd_codec.c`**

`components/obd_codec/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "obd_codec.c" INCLUDE_DIRS "include")
```

`components/obd_codec/obd_codec.c` (core logic — implement fully):
```c
#include "obd_codec.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Extract contiguous hex bytes from ELM text into out[]; returns byte count */
static int extract_hex_bytes(const char *in, uint8_t *out, int max_out)
{
    int n = 0;
    int hi = -1;
    for (const char *p = in; *p && n < max_out; ++p) {
        if (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t' || *p == ':') continue;
        /* skip ISO-TP frame index like "0:" already handled by skipping ':' */
        if (*p >= '0' && *p <= '9' && p[1] == ':') { ++p; continue; }
        int v = hex_nibble(*p);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else {
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    return n;
}

static void dtc_to_string(uint16_t raw, char out[6])
{
    const char *sys = "PCBU";
    out[0] = sys[(raw >> 14) & 0x3];
    out[1] = '0' + ((raw >> 12) & 0x3);
    static const char *hexd = "0123456789ABCDEF";
    out[2] = hexd[(raw >> 8) & 0xF];
    out[3] = hexd[(raw >> 4) & 0xF];
    out[4] = hexd[raw & 0xF];
    out[5] = '\0';
}

bool obd_codec_decode_mode01(const char *response, uint8_t pid, obd_decoded_t *out)
{
    if (!response || !out) return false;
    memset(out, 0, sizeof(*out));
    uint8_t bytes[64];
    int n = extract_hex_bytes(response, bytes, 64);
    int i = 0;
    while (i + 2 < n) {
        if (bytes[i] == 0x41 && bytes[i + 1] == pid) {
            uint8_t A = (i + 2 < n) ? bytes[i + 2] : 0;
            uint8_t B = (i + 3 < n) ? bytes[i + 3] : 0;
            snprintf(out->raw_hex, sizeof(out->raw_hex), "%02X%02X%02X%02X",
                     bytes[i], bytes[i + 1], A, B);
            out->ok = true;
            switch (pid) {
            case 0x0C:
                out->name = "rpm"; out->unit = "rpm";
                out->value = ((A * 256.0) + B) / 4.0; return true;
            case 0x0D:
                out->name = "speed"; out->unit = "km/h";
                out->value = A; return true;
            case 0x05:
                out->name = "coolant_c"; out->unit = "C";
                out->value = (double)A - 40.0; return true;
            case 0x11:
                out->name = "throttle_pct"; out->unit = "%";
                out->value = A * 100.0 / 255.0; return true;
            case 0x2F:
                out->name = "fuel_pct"; out->unit = "%";
                out->value = A * 100.0 / 255.0; return true;
            default:
                out->ok = false; return false;
            }
        }
        ++i;
    }
    return false;
}

int obd_codec_parse_dtcs(const char *response, char out[][6], int max_out)
{
    uint8_t bytes[64];
    int n = extract_hex_bytes(response, bytes, 64);
    int count = 0;
    for (int i = 0; i + 2 < n && count < max_out; ++i) {
        if (bytes[i] == 0x43 || bytes[i] == 0x47 || bytes[i] == 0x4A) {
            for (int j = i + 1; j + 1 < n && count < max_out; j += 2) {
                uint16_t raw = (uint16_t)((bytes[j] << 8) | bytes[j + 1]);
                if (raw == 0) continue;
                dtc_to_string(raw, out[count++]);
            }
            break;
        }
    }
    return count;
}

bool obd_codec_parse_vin(const char *response, char *vin, size_t vin_len)
{
    if (!vin || vin_len < 18) return false;
    uint8_t bytes[64];
    int n = extract_hex_bytes(response, bytes, 64);
    int start = -1;
    for (int i = 0; i + 2 < n; ++i) {
        if (bytes[i] == 0x49 && bytes[i + 1] == 0x02) { start = i + 2; break; }
    }
    if (start < 0) return false;
    /* first data byte after 49 02 is often item count; skip if non-ASCII */
    if (start < n && bytes[start] < 0x20) start++;
    size_t v = 0;
    for (int i = start; i < n && v < 17; ++i) {
        if (bytes[i] >= 0x20 && bytes[i] < 0x7F) vin[v++] = (char)bytes[i];
    }
    vin[v] = '\0';
    return v == 17;
}

bool obd_codec_decode_named(const char *decode_key, const char *response, obd_decoded_t *out)
{
    if (!decode_key || !out) return false;
    if (strcmp(decode_key, "rpm") == 0) return obd_codec_decode_mode01(response, 0x0C, out);
    if (strcmp(decode_key, "speed") == 0) return obd_codec_decode_mode01(response, 0x0D, out);
    if (strcmp(decode_key, "coolant_c") == 0) return obd_codec_decode_mode01(response, 0x05, out);
    if (strcmp(decode_key, "throttle_pct") == 0) return obd_codec_decode_mode01(response, 0x11, out);
    if (strcmp(decode_key, "fuel_pct") == 0) return obd_codec_decode_mode01(response, 0x2F, out);
    if (strcmp(decode_key, "voltage") == 0) {
        memset(out, 0, sizeof(*out));
        out->name = "voltage"; out->unit = "V";
        /* ATRV style: "12.6V" */
        out->value = atof(response);
        out->ok = out->value > 0.0;
        snprintf(out->raw_hex, sizeof(out->raw_hex), "%s", response);
        return out->ok;
    }
    return false;
}
```
- [ ] **Step 5: Run tests — PASS**

```bash
cd tests/host/build && cmake --build . && ctest --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add components/obd_codec tests/host
git commit -m "$(cat <<'EOF'
feat: add obd_codec Mode01/DTC/VIN decode with host tests

EOF
)"
```

---

### Task 4: `elm_transport` + `telemetry_bus` + `sys_runtime`

**Files:**
- Create: `components/elm_transport/include/elm_transport.h`
- Create: `components/elm_transport/CMakeLists.txt` (header-only INTERFACE via dummy.c or just INCLUDE)
- Create: `components/telemetry_bus/include/telemetry_bus.h`
- Create: `components/telemetry_bus/telemetry_bus.c`
- Create: `components/telemetry_bus/CMakeLists.txt`
- Create: `components/sys_runtime/include/sys_runtime.h`
- Create: `components/sys_runtime/sys_runtime.c`
- Create: `components/sys_runtime/CMakeLists.txt`

- [ ] **Step 1: Define `elm_transport.h`**

```c
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef struct elm_transport {
    void *ctx;
    esp_err_t (*write)(struct elm_transport *t, const uint8_t *data, size_t len);
    esp_err_t (*read_line)(struct elm_transport *t, char *buf, size_t buflen, uint32_t timeout_ms);
    bool (*is_ready)(struct elm_transport *t);
} elm_transport_t;
```

`CMakeLists.txt`:
```cmake
idf_component_register(INCLUDE_DIRS "include" REQUIRES esp_common)
```

- [ ] **Step 2: Implement telemetry bus**

Header types matching spec: `TELEMETRY_PID_SAMPLE`, `TELEMETRY_DTC_LIST`, `TELEMETRY_ELM_EVENT`, `TELEMETRY_ERROR`.

```c
esp_err_t telemetry_bus_init(void);
esp_err_t telemetry_subscribe(QueueHandle_t *out_queue, uint32_t filter_mask);
esp_err_t telemetry_publish(const telemetry_msg_t *msg);
```

Implementation notes:
- Max 4 subscribers; each gets a FreeRTOS queue (depth 16).
- `telemetry_publish` copies msg to each matching queue with `xQueueSend` timeout 0; on fail increment drop counter via `sys_runtime_metric_inc("telemetry_drops")`.
- Filter mask bits per message type.

- [ ] **Step 3: Implement `sys_runtime`**

```c
esp_err_t sys_runtime_init(void);          /* start heartbeat task, register with TWDT */
void sys_runtime_metric_inc(const char *name);
uint64_t sys_runtime_metric_get(const char *name);
void sys_runtime_metrics_snapshot_json(char *buf, size_t len);
esp_err_t sys_runtime_ota_stub_status(char *buf, size_t len); /* returns "not_implemented" */
```

Heartbeat task: every 1s `esp_task_wdt_reset()`, log uptime at DEBUG every 60s.

- [ ] **Step 4: Build firmware skeleton still links**

```bash
source ~/esp/esp-idf/export.sh
idf.py build
```

In `app_main`, call `sys_runtime_init()` after NVS init and log `"sys_runtime ready"`.

- [ ] **Step 5: Commit**

```bash
git add components/elm_transport components/telemetry_bus components/sys_runtime main/
git commit -m "$(cat <<'EOF'
feat: add elm_transport, telemetry_bus, and sys_runtime

EOF
)"
```

---

### Task 5: `profile_store` (NVS JSON)

**Files:**
- Create: `components/profile_store/include/profile_store.h`
- Create: `components/profile_store/profile_store.c`
- Create: `components/profile_store/builtin_profiles.h`
- Create: `components/profile_store/CMakeLists.txt`

- [ ] **Step 1: Define API**

```c
typedef struct {
    char cmd[16];
    uint32_t interval_ms;
    char decode[24];
} profile_item_t;

typedef struct {
    char name[32];
    char init_at[8][16];
    int init_at_count;
    profile_item_t items[32];
    int item_count;
} obd_profile_t;

typedef struct {
    uint8_t addr[6];
    bool addr_set;
    char name[32];
    char service_uuid[40];
    char rx_uuid[40];
    char tx_uuid[40];
} ble_bond_t;

esp_err_t profile_store_init(void);
esp_err_t profile_store_get_active(obd_profile_t *out);
esp_err_t profile_store_set_active(const char *name);
esp_err_t profile_store_list(char names[][32], int max, int *count);
esp_err_t profile_store_upsert(const obd_profile_t *p); /* rejects unsafe cmds via cmd_policy */
esp_err_t profile_store_get_bond(ble_bond_t *out);
esp_err_t profile_store_set_bond(const ble_bond_t *b);
esp_err_t profile_store_get_safety(cmd_policy_config_t *out);
esp_err_t profile_store_set_allow_unsafe(bool allow);
```

- [ ] **Step 2: Seed built-in profiles on first boot**

`fleet_basic` and `diagnostics` as in the design spec. Store under NVS namespace `elm` keys: `prof_<name>`, `active`, `bond`, `unsafe`.

Use cJSON; component `REQUIRES nvs_flash json cmd_policy`.

- [ ] **Step 3: On upsert, run every `cmd` and `init_at` through `cmd_policy_is_allowed`; return `ESP_ERR_INVALID_ARG` if any fail.**

- [ ] **Step 4: Smoke from `app_main` — init store, log active profile name.**

- [ ] **Step 5: Build + commit**

```bash
idf.py build
git add components/profile_store main/
git commit -m "$(cat <<'EOF'
feat: add NVS profile_store with builtin fleet profiles

EOF
)"
```

---

### Task 6: `ble_elm` — NimBLE central + NUS transport

**Files:**
- Create: `components/ble_elm/include/ble_elm.h`
- Create: `components/ble_elm/ble_elm.c`
- Create: `components/ble_elm/CMakeLists.txt`

- [ ] **Step 1: Public API**

```c
typedef struct {
    char name[32];
    uint8_t addr[6];
    int8_t rssi;
} ble_elm_device_t;

esp_err_t ble_elm_init(void);
esp_err_t ble_elm_start_scan(uint32_t duration_ms);
esp_err_t ble_elm_get_scan_results(ble_elm_device_t *out, int max, int *count);
esp_err_t ble_elm_connect_addr(const uint8_t addr[6]);
esp_err_t ble_elm_disconnect(void);
bool ble_elm_is_connected(void);
elm_transport_t *ble_elm_get_transport(void); /* valid after connect + GATT discover */
```

Default UUIDs (NUS):
- Service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- RX (write to adapter) `6E400002-...`
- TX (notify from adapter) `6E400003-...`

Load overrides from `profile_store` bond if set.

- [ ] **Step 2: Implement scan/connect/discover/notify ring buffer**

- RX path: GATT notifications append to a FreeRTOS stream buffer or byte queue; `read_line` accumulates until `>` or timeout.
- TX path: `write` sends command bytes + `\r`.
- On disconnect: publish `TELEMETRY_ELM_EVENT` disconnected; clear ready flag.
- Reconnect helper used by poller/app: exponential backoff 1s,2s,4s,... cap 30s to bonded addr.

- [ ] **Step 3: Compile-only verification**

```bash
idf.py build
```

Expected: success. Interactive scan is verified in Task 9/12 after the serial console exists.
- [ ] **Step 4: Commit**

```bash
git add components/ble_elm
git commit -m "$(cat <<'EOF'
feat: add ble_elm NimBLE central with NUS elm_transport

EOF
)"
```

---

### Task 7: `elm327_client` — AT session

**Files:**
- Create: `components/elm327_client/include/elm327_client.h`
- Create: `components/elm327_client/elm327_client.c`
- Create: `components/elm327_client/CMakeLists.txt`

- [ ] **Step 1: Write header and implement client**

`components/elm327_client/include/elm327_client.h`:
```c
#pragma once
#include "elm_transport.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t elm327_client_init(void);
esp_err_t elm327_client_set_transport(elm_transport_t *transport);
esp_err_t elm327_client_run_init_sequence(const char init_at[][16], int count);
esp_err_t elm327_client_transact(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);
bool elm327_client_is_ready(void);
```

Rules in `elm327_client.c`:
- Hold a mutex for the entire `transact`.
- Callers (`obd_poller`, transports) must run `cmd_policy` first.
- Append `\r` if the command does not already end with it.
- Read lines until prompt `>` or timeout.
- If response contains `NO DATA` → return `ESP_ERR_NOT_FOUND`.
- If response contains `UNABLE TO CONNECT` or `ERROR` or `?` → return `ESP_FAIL`.
- `elm327_client_set_transport` is called from the BLE connect path when GATT is ready.

`components/elm327_client/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "elm327_client.c"
                    INCLUDE_DIRS "include"
                    REQUIRES elm_transport freertos esp_common)
```

- [ ] **Step 2: Build**

```bash
idf.py build
```

Expected: success.

- [ ] **Step 3: Commit**

```bash
git add components/elm327_client
git commit -m "$(cat <<'EOF'
feat: add elm327_client AT session over elm_transport

EOF
)"
```

---

### Task 8: `obd_poller`

**Files:**
- Create: `components/obd_poller/include/obd_poller.h`
- Create: `components/obd_poller/obd_poller.c`
- Create: `components/obd_poller/CMakeLists.txt`

- [ ] **Step 1: API**

```c
esp_err_t obd_poller_start(void);
esp_err_t obd_poller_stop(void);
esp_err_t obd_poller_reload_active_profile(void);
esp_err_t obd_poller_submit_raw(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);
```

Behavior:
- FreeRTOS task loops: drain priority raw queue first; else pick next due profile item by `interval_ms`.
- For each cmd: `cmd_policy_check` → if deny, publish error + metric `blocked_cmds`, never call client.
- On allow: `elm327_client_transact` → `obd_codec_decode_named` → `telemetry_publish` pid_sample.
- On BLE not ready: sleep/backoff; do not spam.

- [ ] **Step 2: Build + commit**

```bash
idf.py build
git add components/obd_poller
git commit -m "$(cat <<'EOF'
feat: add profile-driven obd_poller with raw command priority

EOF
)"
```

---

### Task 9: `transport_serial` console

**Files:**
- Create: `components/transport_serial/include/transport_serial.h`
- Create: `components/transport_serial/transport_serial.c`
- Create: `components/transport_serial/CMakeLists.txt`

- [ ] **Step 1: Commands (line-based on UART0 / USB Serial JTAG)**

| Command | Action |
|---|---|
| `help` | list commands |
| `status` | BLE/ELM/profile/metrics summary |
| `scan` | `ble_elm_start_scan` |
| `devices` | print scan results |
| `select <idx>` | bond + connect |
| `cmd <AT/OBD>` | policy → poller raw / client |
| `profiles` | list |
| `profile <name>` | set active + reload poller |
| `telemetry on/off` | dump samples from bus |
| `unsafe on/off` | set NVS flag (loud warning) |
| `metrics` | print counters |

- [ ] **Step 2: Start console task in `transport_serial_start()`; subscribe to telemetry when enabled.**

- [ ] **Step 3: Build + commit**

```bash
idf.py build
git add components/transport_serial
git commit -m "$(cat <<'EOF'
feat: add serial console transport for BLE/OBD control

EOF
)"
```

---

### Task 10: `transport_http` SoftAP + REST + simple UI

**Files:**
- Create: `components/transport_http/include/transport_http.h`
- Create: `components/transport_http/transport_http.c`
- Create: `components/transport_http/http_api.c`
- Create: `components/transport_http/static_index.html.h` (embedded string)
- Create: `components/transport_http/CMakeLists.txt`

- [ ] **Step 1: SoftAP from Kconfig `ELM_SOFTAP_SSID` / `ELM_SOFTAP_PASS`**

- [ ] **Step 2: REST handlers (JSON via cJSON)**

Implement exactly:
- `GET /api/status`
- `POST /api/ble/scan`
- `GET /api/ble/devices`
- `POST /api/ble/select` body `{"index":0}` or `{"addr":"AA:BB:..."}`
- `POST /api/elm/cmd` body `{"cmd":"010C"}` → 403 if policy deny
- `GET /api/profiles`
- `PUT /api/profiles` body full profile JSON
- `POST /api/profiles/active` body `{"name":"fleet_basic"}`
- `GET /api/telemetry`
- `GET /api/metrics`
- `GET /` → HTML

- [ ] **Step 3: Minimal HTML** — scan button, device list, select, live table of last telemetry, raw cmd box. No heavy framework.

- [ ] **Step 4: Build + commit**

```bash
idf.py build
git add components/transport_http
git commit -m "$(cat <<'EOF'
feat: add SoftAP HTTP transport with REST and simple UI

EOF
)"
```

---

### Task 11: Wire `app_main` end-to-end

**Files:**
- Modify: `main/app_main.c`
- Modify: `main/CMakeLists.txt` REQUIRES all components

- [ ] **Step 1: Boot order in `main/app_main.c`**

```c
nvs_flash_init();
sys_runtime_init();
profile_store_init();
telemetry_bus_init();
ble_elm_init();
elm327_client_init();
transport_serial_start();
transport_http_start();

/* ble_elm connect callback must call:
 *   elm327_client_set_transport(ble_elm_get_transport());
 *   then run profile init_at via elm327_client_run_init_sequence()
 */
ble_bond_t bond;
if (profile_store_get_bond(&bond) == ESP_OK && bond.addr_set) {
    ble_elm_connect_addr(bond.addr);
}
obd_poller_start();
```

Update `main/CMakeLists.txt` `REQUIRES` to list: `nvs_flash sys_runtime profile_store telemetry_bus ble_elm elm327_client obd_poller transport_serial transport_http`.

- [ ] **Step 2: Full build**

```bash
source ~/esp/esp-idf/export.sh
idf.py build
```

Expected: success.

- [ ] **Step 3: Commit**

```bash
git add main/
git commit -m "$(cat <<'EOF'
feat: wire app_main boot path for ELM327 bridge

EOF
)"
```

---

### Task 12: On-device bring-up checklist + README

**Files:**
- Create: `README.md`

- [ ] **Step 1: Flash**

```bash
idf.py -p /dev/cu.usbmodem* flash monitor
```

- [ ] **Step 2: Manual checklist (tick in PR/notes)**

1. SoftAP `ELM327-C6` appears; join; open `http://192.168.4.1/`
2. Serial `scan` / web scan lists ELM327
3. Select device; see `init_ok` event
4. `cmd 010C` returns RPM; telemetry updates
5. `cmd 04` → blocked (serial error / HTTP 403); adapter never receives it
6. `cmd 08` → blocked even if `unsafe on`
7. Profile switch `diagnostics` works
8. Power-cycle adapter → auto-reconnect to bonded addr
9. `metrics` shows counters

- [ ] **Step 3: Write README** with hardware notes (BLE-only ELM327 required), build/flash, SoftAP defaults, safety policy summary, UUID override instructions (nRF Connect).

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "$(cat <<'EOF'
docs: add bring-up README for ELM327 ESP32-C6 bridge

EOF
)"
```

---

## Spec coverage checklist

| Spec section | Task(s) |
|---|---|
| BLE central + scan/select/persist | 5, 6, 9, 10, 11 |
| Full AT/OBD behind allowlist | 2, 7, 8, 9, 10 |
| Mode 04/08 blocked | 2, 8, 12 |
| Profiles + poller | 5, 8 |
| Serial + SoftAP transports | 9, 10 |
| Telemetry bus | 4, 8 |
| Watchdog/metrics/OTA stub | 4, 11 |
| Host tests codec/policy | 2, 3 |
| Modular transport boundary | 4, 9, 10 + future stubs noted in README |
| Partition/OTA layout | 1 |

## Execution handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-09-elm327-esp32c6-implementation.md`.

**Two execution options:**

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks, fast iteration  
2. **Inline Execution** — execute tasks in this session with executing-plans, batched checkpoints  

Which approach?
