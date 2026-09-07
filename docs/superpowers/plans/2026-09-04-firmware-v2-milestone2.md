# Firmware v2 Milestone 2 (GPS + time) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable EC200U GNSS + wall-clock (CCLK, GPS backup) already in `lte`, and expose them on the USB CLI without uplink or NVS.

**Architecture:** Flip `CONFIG_LTE_GPS_ENABLE=y`, keep existing `gps_task` / `lte_time_*` paths, add small pure CLI format helpers (host-tested), extend `status` and add `gps`. No new ESP-IDF components; no pin changes.

**Tech Stack:** ESP-IDF 5.2, ESP32-C6, Quectel EC200U AT (`QGPS` / `QGPSLOC` / `CCLK`), USB-Serial/JTAG console, host C tests under `tests/host`.

**Spec:** `docs/superpowers/specs/2026-09-04-firmware-v2-milestone2-design.md`

## Global Constraints

- Branch: `firmware-v2`
- Tree: only `firmware_v2/` (+ host tests under `tests/host` for format helpers); do **not** edit legacy `components/`
- Pins frozen: LTE UART ESP TX=GPIO16, RX=GPIO17
- Time priority: CCLK first, GPS UTC backup (existing `lte` behaviour)
- Product surface: USB only — no NVS last-fix, no telemetry bus, no JSON 1089
- Publish multipart URL must never appear in firmware sources
- Style: short plain names, clear comments, reliable fail paths

## File map

| File | Role |
|------|------|
| `firmware_v2/master/sdkconfig.defaults` | Enable `CONFIG_LTE_GPS_ENABLE=y` |
| `firmware_v2/master/sdkconfig` | Regenerated via `idf.py reconfigure` so GPS is on |
| `firmware_v2/master/components/cli/include/cli_format.h` | Pure format helpers for time/gps lines |
| `firmware_v2/master/components/cli/cli_format.c` | Implementation (no FreeRTOS / no I/O) |
| `firmware_v2/master/components/cli/cli.c` | Wire `status`, `gps`, `help` |
| `firmware_v2/master/components/cli/CMakeLists.txt` | Add `cli_format.c` |
| `tests/host/test_cli_format_v2.c` | Host unit tests for format helpers |
| `tests/host/CMakeLists.txt` | Register the new host test |
| `firmware_v2/master/docs/MASTER.md` | Document `gps` + status lines |
| `firmware_v2/README.md` | Mark M2 current once verified (optional in Task 4) |

No changes expected to `lte.c` / `lte_time.c` unless build proves GPS ifdef nesting still broken (already fixed for `lte_suspend_bg_at` in M1).

---

### Task 1: Enable GNSS in sdkconfig

**Files:**
- Modify: `firmware_v2/master/sdkconfig.defaults`
- Modify: `firmware_v2/master/sdkconfig` (via reconfigure, not hand-edit)

**Interfaces:**
- Consumes: existing `CONFIG_LTE_GPS_ENABLE` in `components/lte/Kconfig`
- Produces: build with GPS task compiled in

- [ ] **Step 1: Update defaults**

In `firmware_v2/master/sdkconfig.defaults`, replace the GPS-off block:

```
# GPS unused in M1 uplink; leave off to free UART during OTA
# CONFIG_LTE_GPS_ENABLE is not set
```

with:

```
# M2: modem GNSS (lat/lng + GPS UTC backup for wall clock)
CONFIG_LTE_GPS_ENABLE=y
```

Leave `CONFIG_LTE_UART_TX_GPIO=16` / `RX=17` unchanged.

- [ ] **Step 2: Reconfigure and confirm**

```bash
cd firmware_v2/master
source ~/esp/esp-idf/export.sh
idf.py reconfigure
rg 'CONFIG_LTE_GPS_ENABLE' sdkconfig sdkconfig.defaults
```

Expected: both files show `CONFIG_LTE_GPS_ENABLE=y` (not `# … is not set`).

- [ ] **Step 3: Build**

```bash
idf.py build
```

Expected: build succeeds; `lte.c` compiles with GPS symbols (no link errors for `lte_gps_get`).

- [ ] **Step 4: Commit**

```bash
git add firmware_v2/master/sdkconfig.defaults
# Prefer not committing machine-local sdkconfig unless the team already tracks it.
git commit -m "feat(v2): enable LTE GNSS for milestone 2"
```

---

### Task 2: CLI format helpers + host tests

**Files:**
- Create: `firmware_v2/master/components/cli/include/cli_format.h`
- Create: `firmware_v2/master/components/cli/cli_format.c`
- Modify: `firmware_v2/master/components/cli/CMakeLists.txt`
- Create: `tests/host/test_cli_format_v2.c`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: `lte_time_t`, `lte_gps_t`, `lte_time_source_t` from `lte.h`; `lte_format_ist` from `lte_time.h`
- Produces:
  - `int cli_format_time_line(const lte_time_t *t, char *out, size_t out_len);`
  - `int cli_format_gps_line(const lte_gps_t *g, char *out, size_t out_len);`
  - Return bytes written excluding NUL, or `-1` on error

- [ ] **Step 1: Write the failing host test**

Create `tests/host/test_cli_format_v2.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cli_format.h"
#include "lte.h"

int main(void)
{
    char buf[128];
    lte_time_t t = {
        .time_ok = true,
        .epoch_ms_utc = 1773997200000ULL, /* 2026-03-20 09:00:00 UTC */
        .source = LTE_TIME_CCLK,
    };
    assert(cli_format_time_line(&t, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "ok=yes") != NULL);
    assert(strstr(buf, "src=cclk") != NULL);
    assert(strstr(buf, "ist=2026-03-20 14:30:00") != NULL);

    lte_time_t none = {0};
    assert(cli_format_time_line(&none, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "ok=no") != NULL);
    assert(strstr(buf, "src=none") != NULL);

    lte_gps_t g = {.gps_ok = true, .lat = 12.9716, .lng = 77.5946, .age_ms = 1500};
    assert(cli_format_gps_line(&g, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "ok=yes") != NULL);
    assert(strstr(buf, "lat=12.971600") != NULL);
    assert(strstr(buf, "lng=77.594600") != NULL);
    assert(strstr(buf, "age_ms=1500") != NULL);

    lte_gps_t bad = {0};
    assert(cli_format_gps_line(&bad, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "ok=no") != NULL);

    assert(cli_format_time_line(NULL, buf, sizeof(buf)) < 0);
    assert(cli_format_gps_line(&g, NULL, 8) < 0);

    printf("test_cli_format_v2: ok\n");
    return 0;
}
```

- [ ] **Step 2: Register host test in `tests/host/CMakeLists.txt`**

Add (mirror `test_net_lte_time` pattern; adjust paths to v2 headers + `lte_time.c` / `cli_format.c`):

```cmake
add_executable(test_cli_format_v2
    test_cli_format_v2.c
    ${CMAKE_SOURCE_DIR}/../firmware_v2/master/components/cli/cli_format.c
    ${CMAKE_SOURCE_DIR}/../firmware_v2/master/components/lte/lte_time.c
)
target_include_directories(test_cli_format_v2 PRIVATE
    ${CMAKE_SOURCE_DIR}/../firmware_v2/master/components/cli/include
    ${CMAKE_SOURCE_DIR}/../firmware_v2/master/components/lte/include
)
add_test(NAME test_cli_format_v2 COMMAND test_cli_format_v2)
```

If `tests/host` `CMAKE_SOURCE_DIR` layout differs, use the same relative style as other tests that reach into `components/` (inspect existing `target_include_directories` and match).

- [ ] **Step 3: Run test — expect fail (missing symbols / headers)**

```bash
cd tests/host
cmake -S . -B build && cmake --build build --target test_cli_format_v2
```

Expected: FAIL (file/symbol not found).

- [ ] **Step 4: Implement helpers**

`firmware_v2/master/components/cli/include/cli_format.h`:

```c
#pragma once

#include "lte.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int cli_format_time_line(const lte_time_t *t, char *out, size_t out_len);
int cli_format_gps_line(const lte_gps_t *g, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
```

`firmware_v2/master/components/cli/cli_format.c`:

```c
#include "cli_format.h"
#include "lte_time.h"

#include <stdio.h>

static const char *time_src_name(lte_time_source_t s)
{
    switch (s) {
    case LTE_TIME_CCLK:
        return "cclk";
    case LTE_TIME_GPS:
        return "gps";
    default:
        return "none";
    }
}

int cli_format_time_line(const lte_time_t *t, char *out, size_t out_len)
{
    if (t == NULL || out == NULL || out_len < 8) {
        return -1;
    }
    char ist[32] = "-";
    if (t->time_ok && t->epoch_ms_utc > 0) {
        if (lte_format_ist(t->epoch_ms_utc, ist, sizeof(ist)) < 0) {
            snprintf(ist, sizeof(ist), "-");
        }
    }
    int n = snprintf(out, out_len, "time: ok=%s src=%s utc_ms=%llu ist=%s",
                     t->time_ok ? "yes" : "no", time_src_name(t->source),
                     (unsigned long long)t->epoch_ms_utc, ist);
    return (n < 0 || (size_t)n >= out_len) ? -1 : n;
}

int cli_format_gps_line(const lte_gps_t *g, char *out, size_t out_len)
{
    if (g == NULL || out == NULL || out_len < 8) {
        return -1;
    }
    int n;
    if (g->gps_ok) {
        n = snprintf(out, out_len, "gps: ok=yes lat=%.6f lng=%.6f age_ms=%u", g->lat,
                     g->lng, (unsigned)g->age_ms);
    } else {
        n = snprintf(out, out_len, "gps: ok=no lat=0.000000 lng=0.000000 age_ms=%u",
                     (unsigned)g->age_ms);
    }
    return (n < 0 || (size_t)n >= out_len) ? -1 : n;
}
```

Update `firmware_v2/master/components/cli/CMakeLists.txt` to include `cli_format.c` and require `lte` (already required if CLI links LTE).

- [ ] **Step 5: Run host test — expect pass**

```bash
cd tests/host
cmake --build build --target test_cli_format_v2
./build/test_cli_format_v2
# or: ctest --test-dir build -R test_cli_format_v2 --output-on-failure
```

Expected: `test_cli_format_v2: ok`

- [ ] **Step 6: Commit**

```bash
git add firmware_v2/master/components/cli/include/cli_format.h \
        firmware_v2/master/components/cli/cli_format.c \
        firmware_v2/master/components/cli/CMakeLists.txt \
        tests/host/test_cli_format_v2.c \
        tests/host/CMakeLists.txt
git commit -m "feat(v2): add CLI time/gps format helpers with host tests"
```

---

### Task 3: Wire USB CLI (`status` + `gps`)

**Files:**
- Modify: `firmware_v2/master/components/cli/cli.c`

**Interfaces:**
- Consumes: `cli_format_time_line`, `cli_format_gps_line`, `lte_time_get`, `lte_gps_get`
- Produces: USB lines matching spec § CLI

- [ ] **Step 1: Extend help**

In `print_help()`, add:

```c
"  gps\n"
```

after the `status` line.

- [ ] **Step 2: Extend `cmd_status`**

After existing LTE/OTA/cfg prints, append (use static buffers for lines; keep large structs static as today):

```c
static lte_time_t tim;
static lte_gps_t gps;
static char line[128];

memset(&tim, 0, sizeof(tim));
memset(&gps, 0, sizeof(gps));
(void)lte_time_get(&tim);
(void)lte_gps_get(&gps);
if (cli_format_time_line(&tim, line, sizeof(line)) > 0) {
    printf("%s\n", line);
}
if (cli_format_gps_line(&gps, line, sizeof(line)) > 0) {
    printf("%s\n", line);
}
```

Include `"cli_format.h"`.

- [ ] **Step 3: Add `cmd_gps` + dispatch**

```c
static void cmd_gps(void)
{
    static lte_gps_t gps;
    static char line[128];
    memset(&gps, 0, sizeof(gps));
    (void)lte_gps_get(&gps);
    if (cli_format_gps_line(&gps, line, sizeof(line)) > 0) {
        printf("%s\n", line);
    }
}
```

In `handle_line`, after `status`:

```c
if (strcmp(line, "gps") == 0) {
    cmd_gps();
    return;
}
```

- [ ] **Step 4: Build firmware**

```bash
cd firmware_v2/master
idf.py build
```

Expected: success.

- [ ] **Step 5: Commit**

```bash
git add firmware_v2/master/components/cli/cli.c
git commit -m "feat(v2): expose time and gps on USB CLI"
```

---

### Task 4: Docs + device verification

**Files:**
- Modify: `firmware_v2/master/docs/MASTER.md`
- Modify: `firmware_v2/README.md` (mark M2 current / M1 done if not already)

**Interfaces:**
- Consumes: Tasks 1–3 behaviour
- Produces: documented commands + lab evidence

- [ ] **Step 1: Update MASTER.md**

USB commands section — add `gps` and note that `status` includes `time:` / `gps:` lines. Mention GPS is on-modem (no extra GPIOs); OTA still suspends background AT.

- [ ] **Step 2: Flash and verify time (required)**

```bash
cd firmware_v2/master
idf.py -p /dev/cu.usbmodem1101 flash
# then USB:
# status
```

Expected after LTE register (~tens of seconds):

- `time: ok=yes src=cclk … ist=…` (IST wall clock plausible)
- `gps: ok=no …` acceptable indoors

- [ ] **Step 3: Optional outdoor GPS**

Near window/outside, wait for fix, run `gps` / `status`. Expected: `gps: ok=yes` with sensible lat/lng and `age_ms` under `CONFIG_LTE_GPS_MAX_AGE_S`.

- [ ] **Step 4: OTA smoke with GPS on**

```text
ota check
```

Expected: check completes (`no_update` if already latest, or download path healthy). No UART deadlock / stack panic. Confirm logs show `background AT suspended (OTA)` then resumed.

- [ ] **Step 5: Commit docs**

```bash
git add firmware_v2/master/docs/MASTER.md firmware_v2/README.md
git commit -m "docs(v2): document milestone 2 GPS/time CLI"
```

---

## Spec coverage (self-review)

| Spec requirement | Task |
|------------------|------|
| `CONFIG_LTE_GPS_ENABLE=y` | Task 1 |
| CCLK first / GPS backup (existing) | Task 1 (enable path); no code change |
| USB `status` time + gps lines | Tasks 2–3 |
| USB `gps` command | Task 3 |
| Indoor time required / GPS optional | Task 4 |
| Outdoor nice-to-have | Task 4 Step 3 |
| OTA smoke with GPS on | Task 4 Step 4 |
| No publish URL / no pin remap / no legacy edits | Global Constraints + all tasks |

Placeholder scan: none. Format helper signatures consistent across Tasks 2–3.
