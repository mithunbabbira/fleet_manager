# Session review — changes made 2026-09-02

Use this document on another machine to compare against your last known-good tree.
It lists **every file touched** in this work session, **why** it changed, what was tried and
reverted, and what still needs bench verification.

**Target hardware:** ESP32-C6 Super Mini on printed carrier PCB only.  
**LTE UART (confirmed correct):** `CONFIG_NET_LTE_UART_TX_GPIO=16`, `CONFIG_NET_LTE_UART_RX_GPIO=17`

---

## Quick status (as of last flash on homeserver)

| Area | Status |
|------|--------|
| Boot / no reboot loop | **Fixed** — device reaches `obd>` console |
| Zigbee coordinator | **OK** — network steering, permit join |
| LTE UART driver init | **OK** — `UART1 ready TX=GPIO16 RX=GPIO17` |
| LTE modem AT | **Not confirmed** — `no AT yet` / no RX bytes (see §5) |
| MCP2515 / OBD | Expected fail on bare bench without CAN harness |
| microSD queue | Expected fail without card inserted |
| Uplink | Waiting on `provision` + `uplink on` + LTE registration |

---

## 1. Critical boot-loop fix (`net_lte.c`)

### Symptom
After `idf.py set-target` and flashing new firmware, device **rebooted forever** with:

```
Guru Meditation Error: Interrupt wdt timeout on CPU0
uart_ll_update() → uart_driver_install(UART_NUM_1)
```

Crash happened right after `telemetry_bus ready`, inside `net_lte_start()` → `uart_init()`.

### Root cause
On **ESP32-C6**, after Zigbee / coexist init, **UART1 core clock (`sclk_en`)** can be gated.
`uart_driver_install()` calls `uart_ll_update()` which spins forever if the core clock is off.

This is **not** caused by telemetry JSON changes — crash occurs before any uplink code runs.

### Fix (keep this)
In `components/net_lte/net_lte.c`, `uart_init()` — **before** `uart_driver_install()`:

```c
#include "esp_private/uart_share_hw_ctrl.h"
#include "hal/uart_ll.h"
#include "soc/soc_caps.h"

/* ESP32-C6: Zigbee/coex can leave UART1 core clock off; driver install hangs without this. */
if (CONFIG_NET_LTE_UART_PORT < SOC_UART_HP_NUM) {
    HP_UART_BUS_CLK_ATOMIC() {
        uart_ll_enable_bus_clock(CONFIG_NET_LTE_UART_PORT, true);
    }
    HP_UART_SRC_CLK_ATOMIC() {
        uart_dev_t *hw = UART_LL_GET_HW(CONFIG_NET_LTE_UART_PORT);
        uart_ll_sclk_enable(hw);
        uart_ll_set_sclk(hw, UART_SCLK_DEFAULT);
    }
}
```

UART init order remains standard ESP-IDF: `uart_driver_install` → `uart_param_config` → `uart_set_pin`.
**Do not reorder** — IDF examples use this sequence; reordering did not fix the hang.

### What was tried and reverted (do not re-apply)
- Moving `uart_param_config()` before `uart_driver_install()` — crash moved but still hung.
- Moving LTE init **before** Zigbee in `app_main.c` — did not fix boot loop; **reverted** to Zigbee → LTE order.

---

## 2. LTE modem bring-up changes (`net_lte.c`)

### GPS task timing (new in this session)
**Problem:** `gps_task` was started in `net_lte_start()` immediately after UART init. It sent
`AT+QGPS=1` while `bringup_task` was still probing `AT`, competing for the same UART mutex.
Old `sdkconfig.old` had **no** `CONFIG_NET_LTE_GPS_ENABLE` — GPS did not exist in the previous build.

**Fix:** Start GPS only **after** first successful `AT` in `bringup_task()`:

- Removed `xTaskCreate(gps_task, ...)` from `net_lte_start()`
- Added `gps_task_start()` called from `bringup_task()` when `at_ok == true`
- Forward declaration: `static void gps_task_start(void);`

### AT retry window
- `NET_LTE_AT_ATTEMPTS`: **40 → 60** (30 s total at 500 ms interval)
- EC200U cold boot after flash can take 30–60 s.

### Unchanged (still correct)
- `uart_set_pin(TX=CONFIG_NET_LTE_UART_TX_GPIO, RX=CONFIG_NET_LTE_UART_RX_GPIO)` — GPIO **16/17**
- `at_transact()` still sends `CMD\r` (Quectel standard)
- Background `bringup_task` + mutex-serialized AT

---

## 3. Boot order (`main/app_main.c`)

**Current order** (restored to original):

1. NVS, sys_runtime, fw_ota*, profile_store, telemetry_bus  
2. **Zigbee** (`transport_zigbee_init` / `start`) — non-fatal on failure  
3. **LTE** (`net_lte_start`, `fw_ota_lte_start_auto`) — non-fatal on failure  
4. SPI lock, CAN/OBD, SD, uplink, serial console, OTA confirm, poller  

Zigbee before LTE matches pre-session layout. Incorrect comment about “UART clock gated by Zigbee” was removed.

---

## 4. Uplink JSON redesign (major feature — earlier in session)

Goal: replace monolithic telemetry JSON with **one typed event per reading**, shared envelope:

```json
{
  "device_id": "...",
  "node_id": "...",
  "schemaId": "1087",
  "ts_ms": 1735689600000,
  "payload": { ... }
}
```

### New files
| File | Purpose |
|------|---------|
| `components/telemetry_uplink/include/uplink_schema_ids.h` | Build-time schema IDs: OBD `1087`, host UL212 `1088`, GPS `1089` |
| `components/telemetry_uplink/uplink_schema.c` | `uplink_schema_for_host(host_type_id)` mapping |
| `components/telemetry_uplink/include/uplink_schema.h` | Declarations + includes schema IDs header |

### Modified — telemetry uplink
| File | Changes |
|------|---------|
| `components/telemetry_uplink/include/uplink_payload.h` | `uplink_event_t`, `uplink_emit_ctx_t`, multi-event APIs |
| `components/telemetry_uplink/uplink_payload.c` | Split builders: OBD, GPS, host reading; envelope validation; batch/live serialize |
| `components/telemetry_uplink/telemetry_uplink.c` | Multi-event produce/drain; `s_io_mu` mutex; `event_ts_ms()` wall clock; SD one-line-per-event |
| `components/telemetry_uplink/include/telemetry_uplink.h` | Updated prototypes |
| `components/telemetry_uplink/CMakeLists.txt` | Added `uplink_schema.c` |

### Live POST body shape
- **1 event** → single JSON object  
- **N events** → JSON array  

### Schema IDs (edit before OTA in `uplink_schema_ids.h`)
```c
#define UPLINK_SCHEMA_OBD        "1087"
#define UPLINK_SCHEMA_HOST_UL212 "1088"
#define UPLINK_SCHEMA_GPS        "1089"
```

---

## 5. Wall-clock time (`net_lte.c` / `net_lte.h`)

Added UTC `ts_ms` for uplink events:

- `net_lte_time_now_ms()`, `net_lte_time_get()`
- Sources: GPS UTC (preferred), then `AT+CCLK` (assume IST +22 quarters if no TZ), then uptime fallback
- India-only: single TZ handling via `NET_LTE_TZ_QUARTERS_IST`
- `s_time_mutex`, plausibility window 2025–2100
- GPS time wins over CCLK once set
- `s_suspend_bg_at` — skip background AT during OTA UART ownership

---

## 6. Stability / reliability fixes (earlier in session)

### `components/store_sd/store_sd.c`
- Clear `s_mounted` after repeated I/O failures → uplink falls back to live POST
- Safer compact (rename before unlink)
- Reconcile queue meta with file on init
- Bounds-check `ack` head; robust `drop_oldest` on partial lines

### `components/telemetry_uplink/telemetry_uplink.c`
- `s_io_mu` mutex — serialize `produce_once` and `drain_once` (shared static buffers)
- Skip corrupt SD queue lines instead of stalling drain

### `components/host_registry/host_registry.c`
- Evict LRU host when table full (`FLEET_REGISTRY_MAX_HOSTS`)

### `components/telemetry_bus/include/telemetry_bus.h`
- Increased `telemetry_host_reading_t.key` and `.host_type` buffer sizes (match manifest — avoid silent truncation)

### `main/app_main.c`
- Zigbee init **non-fatal**
- `can_obd_start()` **non-fatal** (vehicle bus optional on bench)

### `components/net_lte/net_lte.c`
- Skip `net_lte_refresh()` AT traffic while OTA owns UART (`s_suspend_bg_at`)

### `tests/host/CMakeLists.txt`
- `test_fw_ota_lte_parse` only if `IDF_PATH` set

### `tests/host/test_uplink_payload.c`
- Assertions for new envelope; fixed `snap->field` pointer bugs

---

## 7. Documentation updated

| File | Changes |
|------|---------|
| `docs/telemetry-api-backend-guide.md` | Multi-event model, schema IDs, `ts_ms` sourcing |
| `PROJECT_CONTEXT.md` | Multi-event uplink, wall-clock `ts_ms` |

---

## 8. `sdkconfig` / build config (important)

Running `idf.py set-target` **regenerated** `sdkconfig` from `sdkconfig.defaults`.
Previous live config saved as **`sdkconfig.old`** in repo root (gitignored).

### Confirmed correct (do not change)
```
CONFIG_NET_LTE_UART_TX_GPIO=16
CONFIG_NET_LTE_UART_RX_GPIO=17
```

### `sdkconfig.old` had (stale — do NOT restore for GPIO)
```
CONFIG_NET_LTE_UART_TX_GPIO=17   # wrong for printed PCB
CONFIG_NET_LTE_UART_RX_GPIO=16
```

### Other `sdkconfig.old` vs new differences
| Item | Old | New |
|------|-----|-----|
| BLE NimBLE | enabled | disabled (`CONFIG_BT_ENABLED` not set) |
| Zigbee | not in component config | `CONFIG_ZB_ENABLED=y`, `CONFIG_FLEET_ZIGBEE_ENABLE=y` |
| GPS | not configured | `CONFIG_NET_LTE_GPS_ENABLE=y` |
| OTA rollback | off | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` |

**Note:** Editing `sdkconfig.defaults` alone does **not** update an existing `sdkconfig`.
Use `idf.py menuconfig` or edit `sdkconfig` directly, then rebuild.

---

## 9. Why LTE AT may still fail (hardware / bench)

GPIO 16/17 are **correct** per PCB README. UART driver init succeeds. **No RX bytes** means:

1. EC200U **VBAT** not powered (needs separate supply, not ESP USB alone)
2. **PWRKEY** not toggled — modem not booted
3. **NETLIGHT** — should blink when modem is alive
4. **Cold boot** — wait 30–60 s after power-on / flash reset
5. After GPS defer fix — rebuild and retest before assuming hardware fault

Console checks:
```
lte
provision <device_id> <node_id>
uplink on
```

---

## 10. Full file list (git diff scope)

Modified (`M`):
- `PROJECT_CONTEXT.md`
- `components/host_registry/fleet_manifest_catalog.c`
- `components/host_registry/host_registry.c`
- `components/net_lte/include/net_lte.h`
- `components/net_lte/net_lte.c`
- `components/store_sd/store_sd.c`
- `components/telemetry_bus/include/telemetry_bus.h`
- `components/telemetry_uplink/CMakeLists.txt`
- `components/telemetry_uplink/include/telemetry_uplink.h`
- `components/telemetry_uplink/include/uplink_payload.h`
- `components/telemetry_uplink/telemetry_uplink.c`
- `components/telemetry_uplink/uplink_payload.c`
- `docs/telemetry-api-backend-guide.md`
- `main/app_main.c`
- `tests/host/CMakeLists.txt`
- `tests/host/test_fleet_uplink_path.c`
- `tests/host/test_uplink_payload.c`

New (`??`):
- `components/telemetry_uplink/include/uplink_schema.h`
- `components/telemetry_uplink/include/uplink_schema_ids.h`
- `components/telemetry_uplink/uplink_schema.c`
- `docs/REVIEW_2026-09-02-session-changes.md` (this file)

Not for commit:
- `tempFolder/temp.txt` — scratch, exclude from branch

---

## 11. Build & test on other machine

```bash
source ~/esp/esp-idf/export.sh
cd fleet_manager

# Host tests (no IDF full build required for most)
cmake -S tests/host -B build-host && cmake --build build-host && ctest --test-dir build-host

# Firmware
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### Expected good boot log (abbreviated)
```
telemetry_bus ready
transport_zigbee ready
net_lte: UART1 ready TX=GPIO16 RX=GPIO17 baud=115200
net_lte: AT OK after N attempt(s)    ← need this for LTE
transport_serial started
obd>
```

### Run host tests
```bash
# from tests/host build dir
./test_uplink_payload
./test_fleet_uplink_path
```

---

## 12. Suggested review order on other PC

1. **`net_lte.c` `uart_init()`** — clock enable block (§1); keep it.
2. **`net_lte.c` `gps_task_start()`** — only after AT OK (§2).
3. **`app_main.c`** boot order — Zigbee then LTE (§3).
4. **`uplink_payload.c` / `telemetry_uplink.c`** — JSON shape if backend team needs samples (`docs/telemetry-api-backend-guide.md`).
5. **`sdkconfig`** — confirm GPIO 16/17, Zigbee enabled, GPS enabled.
6. Bench: modem power → `lte` command → provision → `uplink on`.

---

## 13. Git — new branch push (run on machine with git access)

```bash
cd fleet_manager
git checkout -b review/2026-09-02-uplink-stability-lte-fix

git add \
  PROJECT_CONTEXT.md \
  components/ \
  docs/telemetry-api-backend-guide.md \
  docs/REVIEW_2026-09-02-session-changes.md \
  main/app_main.c \
  tests/host/

# Do NOT add tempFolder/
git status

git commit -m "$(cat <<'EOF'
Add multi-event uplink, stability fixes, and ESP32-C6 UART1 boot fix.

Boot loop on UART1 init is fixed for C6+Zigbee; GPS AT deferred until modem
AT OK; uplink emits typed schema events with wall-clock ts_ms.
EOF
)"

git push -u origin review/2026-09-02-uplink-stability-lte-fix
```

---

## 14. Agent environment note

Shell/git from Cursor agent on `homeserver` was blocked by `intuit-git-push-guard`
(Artifactory VPN). Builds were run manually in user terminal. Use another machine for
`git push` if hooks block there too.

---

*Generated for handoff review — 2026-09-02.*
