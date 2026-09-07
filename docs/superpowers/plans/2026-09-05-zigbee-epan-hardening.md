# Zigbee EPAN Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Carrier and UL212 host share a fixed Extended PAN ID + channel so only matching firmware joins; permit-join stays forever-open.

**Architecture:** Parse a 16-hex-digit EPAN (MSB-first display form) into little-endian `esp_zb_ieee_addr_t`, call `esp_zb_set_extended_pan_id()` on the coordinator before formation and on the ED before join/steer. No install codes, no permit-join CLI changes.

**Tech Stack:** ESP-IDF Zigbee (`esp-zigbee-lib`) on `firmware_v2/master`; PlatformIO Arduino Zigbee ED + FleetZigbee on `ul212-rs232-fetch`.

**Spec:** `docs/superpowers/specs/2026-09-05-zigbee-epan-hardening-design.md`

## Global Constraints

- Branch: `firmware-v2`
- Do **not** edit legacy `components/`
- Lab defaults: channel **15**, EPAN display string **`F1EE700000000001`** (same on master Kconfig defaults and host `zigbee_app_config.h`)
- Permit-join stays forever-open (255s + refresh) — do not add timed join / `zb permit`
- No install codes (`install_code_policy` stays `false`)
- Do **not** `git commit` unless the user explicitly asks
- After first flash with a new EPAN, **erase flash once** on both boards (Zigbee NVS keeps the old network otherwise)
- Bump master `VERSION` once when radio/config lands (e.g. `1.0.22` → `1.0.23`)

## File map

| File | Responsibility |
|------|----------------|
| `firmware_v2/master/components/transport_zigbee/Kconfig` | `FLEET_ZIGBEE_EPAN_ID` string |
| `firmware_v2/master/sdkconfig.defaults` | Lab EPAN default |
| `firmware_v2/master/components/transport_zigbee/include/fleet_zb_epan.h` | Shared hex→LE parse + format helpers |
| `firmware_v2/master/components/transport_zigbee/fleet_zb_epan.c` | Helper implementation |
| `firmware_v2/master/components/transport_zigbee/transport_zigbee_radio.c` | Set EPAN before start; log EPAN on network up |
| `firmware_v2/master/components/transport_zigbee/CMakeLists.txt` | Compile `fleet_zb_epan.c` |
| `firmware_v2/master/VERSION` | Patch bump |
| `hardware/.../ul212-rs232-fetch/include/zigbee_app_config.h` | `FLEET_ZB_EPAN_ID` macro |
| `hardware/.../host/lib/FleetZigbee/src/fleet_zigbee_ed.cpp` | Set EPAN before `Zigbee.begin()` |
| `hardware/.../ul212-rs232-fetch/README.md` | Document channel+EPAN truck template |
| `firmware_v2/host/HOW_TO_MAKE_A_HOST.md` | Same multi-truck note if it still says channel-only |
| `tests/host/test_fleet_zb_epan.c` | Host unit test for parse/format |

**EPAN encoding (lock this everywhere):**

- Config string: 16 hex digits, **MSB-first display form**, e.g. `F1EE700000000001`
- API bytes (`esp_zb_ieee_addr_t`): **little-endian**, so that string becomes  
  `{ 0x01, 0x00, 0x00, 0x00, 0x00, 0x70, 0xEE, 0xF1 }`
- Logs print MSB-first (`%02x%02x…` from index 7 down to 0), matching Espressif examples

---

### Task 1: EPAN parse helper + host test

**Files:**
- Create: `firmware_v2/master/components/transport_zigbee/include/fleet_zb_epan.h`
- Create: `firmware_v2/master/components/transport_zigbee/fleet_zb_epan.c`
- Modify: `firmware_v2/master/components/transport_zigbee/CMakeLists.txt`
- Create: `tests/host/test_fleet_zb_epan.c`
- Modify: `tests/host/CMakeLists.txt` (or existing host-test runner) to build/run this test

**Interfaces:**
- Produces:
  - `bool fleet_zb_epan_parse(const char *hex16, uint8_t out_le[8]);` — accepts optional `0x` prefix; requires exactly 16 hex digits; writes LE bytes; returns false on bad input (leaves `out_le` unchanged on failure)
  - `void fleet_zb_epan_format(const uint8_t in_le[8], char out[17]);` — writes 16 uppercase hex chars + NUL, MSB-first

- [ ] **Step 1: Write the failing host test**

Create `tests/host/test_fleet_zb_epan.c`:

```c
#include "fleet_zb_epan.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void expect_true(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        fails++;
    }
}

int main(void)
{
    uint8_t le[8];
    char fmt[17];

    expect_true(fleet_zb_epan_parse("F1EE700000000001", le), "parse ok");
    expect_true(le[0] == 0x01 && le[1] == 0x00 && le[2] == 0x00 && le[3] == 0x00 &&
                    le[4] == 0x00 && le[5] == 0x70 && le[6] == 0xEE && le[7] == 0xF1,
                "LE bytes");

    fleet_zb_epan_format(le, fmt);
    expect_true(strcmp(fmt, "F1EE700000000001") == 0, "round-trip format");

    expect_true(!fleet_zb_epan_parse("short", le), "reject short");
    expect_true(!fleet_zb_epan_parse("F1EE70000000000G", le), "reject non-hex");
    expect_true(fleet_zb_epan_parse("0xF1EE700000000001", le), "accept 0x prefix");

    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    puts("ok");
    return 0;
}
```

Wire into `tests/host/CMakeLists.txt` (same pattern as `test_uplink_host_v2`):

```cmake
add_executable(test_fleet_zb_epan
    test_fleet_zb_epan.c
    ${CMAKE_SOURCE_DIR}/../../firmware_v2/master/components/transport_zigbee/fleet_zb_epan.c
)
target_include_directories(test_fleet_zb_epan PRIVATE
    ${CMAKE_SOURCE_DIR}/../../firmware_v2/master/components/transport_zigbee/include
)
add_test(NAME test_fleet_zb_epan COMMAND test_fleet_zb_epan)
```

- [ ] **Step 2: Run test — expect fail (missing sources)**

```bash
cd /Users/nc23896-mithun/Documents/office/nc_rnd/fleet/ELM327_esp/tests/host
cmake -S . -B build-epan && cmake --build build-epan --target test_fleet_zb_epan
```

Expected: configure/build **FAIL** (`fleet_zb_epan.c` / header missing).

- [ ] **Step 3: Implement helper**

`fleet_zb_epan.h`:

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Parse 16 hex digits (optional 0x) MSB-first into little-endian 8 bytes. */
bool fleet_zb_epan_parse(const char *hex16, uint8_t out_le[8]);

/** Format LE 8 bytes to 16 uppercase hex chars + NUL (MSB-first). */
void fleet_zb_epan_format(const uint8_t in_le[8], char out[17]);

#ifdef __cplusplus
}
#endif
```

`fleet_zb_epan.c`: implement with a small nibble parser; skip leading `0x`/`0X`; reject wrong length or non-hex; on success fill `out_le[0]=LSB … out_le[7]=MSB`.

Add `fleet_zb_epan.c` to `transport_zigbee` `CMakeLists.txt` `SRCS` (always, even when radio is disabled — pure C, no Zigbee deps).

- [ ] **Step 4: Re-run host test — expect pass**

```bash
cd /Users/nc23896-mithun/Documents/office/nc_rnd/fleet/ELM327_esp/tests/host
cmake -S . -B build-epan && cmake --build build-epan --target test_fleet_zb_epan
./build-epan/test_fleet_zb_epan
```

Expected: prints `ok`, exit 0.

- [ ] **Step 5: Commit only if user asked**

Otherwise stop here for this task’s review gate.

---

### Task 2: Carrier Kconfig + set EPAN on form

**Files:**
- Modify: `firmware_v2/master/components/transport_zigbee/Kconfig`
- Modify: `firmware_v2/master/sdkconfig.defaults`
- Modify: `firmware_v2/master/components/transport_zigbee/transport_zigbee_radio.c`
- Modify: `firmware_v2/master/components/transport_zigbee/transport_zigbee.c` (log line if it mentions channel-only)
- Modify: `firmware_v2/master/VERSION`

**Interfaces:**
- Consumes: `fleet_zb_epan_parse`, `fleet_zb_epan_format`, `CONFIG_FLEET_ZIGBEE_EPAN_ID`
- Produces: coordinator network formed with fixed EPAN; forever-open permit-join unchanged

- [ ] **Step 1: Add Kconfig + defaults**

In `Kconfig`, under `FLEET_ZIGBEE_CHANNEL`:

```kconfig
config FLEET_ZIGBEE_EPAN_ID
    string "Extended PAN ID (16 hex digits, MSB-first)"
    depends on FLEET_ZIGBEE_ENABLE
    default "F1EE700000000001"
    help
        Must match the host FLEET_ZB_EPAN_ID. Unique per truck together with channel.
        Changing this requires erase-flash once so Zigbee NVS does not keep the old network.
```

In `sdkconfig.defaults`:

```
CONFIG_FLEET_ZIGBEE_EPAN_ID="F1EE700000000001"
```

If a local `sdkconfig` already exists without this symbol, run `idf.py reconfigure` or add the same line so the build picks it up.

- [ ] **Step 2: Apply EPAN in `coordinator_task` before `esp_zb_start`**

In `transport_zigbee_radio.c`, `#include "fleet_zb_epan.h"`. After `esp_zb_init` / channel mask, before `esp_zb_start(false)`:

```c
    esp_zb_ieee_addr_t epan = {0};
    if (!fleet_zb_epan_parse(CONFIG_FLEET_ZIGBEE_EPAN_ID, epan)) {
        ESP_LOGE(TAG, "bad CONFIG_FLEET_ZIGBEE_EPAN_ID='%s'", CONFIG_FLEET_ZIGBEE_EPAN_ID);
        vTaskDelete(NULL);
        return;
    }
    esp_zb_set_extended_pan_id(epan);
    {
        char epan_str[17];
        fleet_zb_epan_format(epan, epan_str);
        ESP_LOGI(TAG, "EPAN=%s ch=%d (factory-new forms this network)", epan_str,
                 CONFIG_FLEET_ZIGBEE_CHANNEL);
    }
```

On `ESP_ZB_BDB_SIGNAL_FORMATION` success (and optionally reboot steering success), enrich the log:

```c
            esp_zb_ieee_addr_t cur = {0};
            char epan_str[17];
            esp_zb_get_extended_pan_id(cur);
            fleet_zb_epan_format(cur, epan_str);
            ESP_LOGI(TAG, "network up PAN=0x%04x EPAN=%s ch=%d", esp_zb_get_pan_id(), epan_str,
                     esp_zb_get_current_channel());
```

Do **not** change `permit_join_open` / refresh behavior.

Update file header comment: form network on channel **+ EPAN**, still open join.

- [ ] **Step 3: Bump VERSION**

Set `firmware_v2/master/VERSION` to `1.0.23`.

- [ ] **Step 4: Build master**

```bash
cd /Users/nc23896-mithun/Documents/office/nc_rnd/fleet/ELM327_esp/firmware_v2/master
idf.py build
```

Expected: build succeeds; no new warnings treated as errors.

- [ ] **Step 5: Commit only if user asked**

---

### Task 3: Host config + ED set EPAN before join

**Files:**
- Modify: `hardware/fleet_telematics_carrier/host/ul212-rs232-fetch/include/zigbee_app_config.h`
- Modify: `hardware/fleet_telematics_carrier/host/lib/FleetZigbee/src/fleet_zigbee_ed.cpp`
- Modify: `hardware/fleet_telematics_carrier/host/ul212-rs232-fetch/README.md`
- Modify: `firmware_v2/host/HOW_TO_MAKE_A_HOST.md` (if it documents channel-only isolation)

**Interfaces:**
- Consumes: `FLEET_ZB_EPAN_ID` string macro; `esp_zb_set_extended_pan_id`
- Produces: ED prefers / joins only the configured EPAN on `FLEET_ZB_CHANNEL`

- [ ] **Step 1: Add EPAN to `zigbee_app_config.h`**

Update the header comment to say truck isolation = **same channel + same EPAN** as that truck’s carrier. Add:

```c
/* 16 hex digits, MSB-first — must match CONFIG_FLEET_ZIGBEE_EPAN_ID on the carrier. */
#ifndef FLEET_ZB_EPAN_ID
#define FLEET_ZB_EPAN_ID "F1EE700000000001"
#endif
```

Keep existing channel / device_id macros unchanged.

- [ ] **Step 2: Set EPAN in `radioStart()` before `Zigbee.begin()`**

In `fleet_zigbee_ed.cpp`, add a tiny local parse (duplicate of the 16-hex logic is OK to avoid pulling IDF component paths into PlatformIO — keep it ≤40 lines) **or** copy `fleet_zb_epan.c/.h` into `host/lib/FleetZigbee` if that is cleaner. Prefer **copying the two helper files** into:

- `hardware/fleet_telematics_carrier/host/lib/FleetZigbee/include/fleet_zb_epan.h`
- `hardware/fleet_telematics_carrier/host/lib/FleetZigbee/src/fleet_zb_epan.c`

Keep master and host copies identical (same test vectors).

In `radioStart()` after `setPrimaryChannelMask`, before `Zigbee.begin()`:

```cpp
    esp_zb_ieee_addr_t epan = {};
    if (!fleet_zb_epan_parse(FLEET_ZB_EPAN_ID, epan)) {
        Serial.printf("[zb] bad FLEET_ZB_EPAN_ID=%s\n", FLEET_ZB_EPAN_ID);
        delay(1000);
        ESP.restart();
    }
    esp_zb_set_extended_pan_id(epan);
    char epan_str[17];
    fleet_zb_epan_format(epan, epan_str);
    Serial.printf("[zb] joining coordinator (ch %d EPAN=%s)…\n", FLEET_ZB_CHANNEL, epan_str);
```

Ensure `#include "fleet_zb_epan.h"` and that PlatformIO builds `fleet_zb_epan.c` (FleetZigbee lib `src/` is usually auto-globbed).

Also call `esp_zb_set_extended_pan_id(epan)` inside `requestNetworkSteering()` **before** `esp_zb_bdb_start_top_level_commissioning` (after lock), so reconnect uses the same preferred EPAN.

- [ ] **Step 3: Docs**

In `ul212-rs232-fetch/README.md` and `HOW_TO_MAKE_A_HOST.md`:

- Truck template: unique **channel and EPAN**
- Lab default EPAN `F1EE700000000001`
- Note: erase-flash once after EPAN change
- Point multi-truck readers at `docs/superpowers/specs/2026-09-05-zigbee-epan-hardening-design.md`

- [ ] **Step 4: Build host firmware**

```bash
cd /Users/nc23896-mithun/Documents/office/nc_rnd/fleet/ELM327_esp/hardware/fleet_telematics_carrier/host/ul212-rs232-fetch
pio run
```

Expected: build succeeds.

- [ ] **Step 5: Commit only if user asked**

---

### Task 4: Flash + hardware verify + ngrok uplink

**Files:** none (ops / lab)

**Interfaces:**
- Consumes: flashed master `1.0.23` + host with matching EPAN; mock + ngrok from prior lab flow

- [ ] **Step 1: Erase + flash both boards (EPAN change needs clean Zigbee NVS)**

Ports from last lab (confirm with `ls /dev/cu.usbmodem*`):

- Carrier: often `/dev/cu.usbmodem1101`
- Host: often `/dev/cu.usbmodem101`

```bash
cd /Users/nc23896-mithun/Documents/office/nc_rnd/fleet/ELM327_esp/firmware_v2/master
idf.py -p /dev/cu.usbmodem1101 erase-flash flash monitor

# other terminal
cd /Users/nc23896-mithun/Documents/office/nc_rnd/fleet/ELM327_esp/hardware/fleet_telematics_carrier/host/ul212-rs232-fetch
pio run -t erase -t upload && pio device monitor -b 115200
```

Expected master log: `EPAN=F1EE700000000001 ch=15`, then `network up … EPAN=F1EE700000000001`.  
Expected host: `[zb] joining … EPAN=F1EE700000000001`, then `[zb] joined, HELLO sent`.  
Carrier CLI: `fleet hosts` shows `ul212-rs232-001` with link up.

- [ ] **Step 2: Negative check (wrong EPAN)**

Temporarily set host `#define FLEET_ZB_EPAN_ID "F1EE700000000002"`, rebuild, erase+flash host only.  
Expected: host does **not** join; `fleet hosts` stays without a live link (or link drops).  
Restore lab EPAN, erase+flash host again, confirm rejoin.

- [ ] **Step 3: Ngrok uplink smoke**

```bash
cd /Users/nc23896-mithun/Documents/office/nc_rnd/fleet/ELM327_esp/tools/telemetry_mock
# start server.py (Vehicle wrap) + ngrok http <mock-port>
```

On carrier CLI: `uplink url <ngrok-https-url>` then `uplink once` / wait for tick.  
Expected: mock receives batch with schema **1088** (and **1089** if GPS has a fix).

- [ ] **Step 4: Restore Trafyn URL if that was the pre-lab setting** (optional; ask user)

- [ ] **Step 5: Commit only if user asked**

---

## Spec coverage (self-review)

| Spec requirement | Task |
|------------------|------|
| Fixed EPAN + channel config both sides | 2, 3 |
| Carrier forms configured EPAN; log EPAN | 2 |
| Forever-open permit-join unchanged | 2 (explicit non-change) |
| Host joins only matching EPAN | 3, 4.2 |
| Lab default shared | 2, 3 |
| No install codes / permit CLI | Global + Task 2 |
| Join success + wrong-EPAN fail + ngrok 1088 | 4 |
| Docs channel+EPAN truck template | 3 |

## Placeholder scan

None. Host-test wiring matches `tests/host/CMakeLists.txt` (`test_uplink_host_v2` pattern).
