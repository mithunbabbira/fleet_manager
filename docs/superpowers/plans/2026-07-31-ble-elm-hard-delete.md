# BLE/ELM Hard Delete Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove BLE ELM327 Mini stack from the fleet telematics firmware so NimBLE is not linked; SoftAP + CAN + LTE remain.

**Architecture:** Hard-delete `ble_elm`, `elm327_client`, and `elm_transport`; rewire serial/HTTP to `can_obd`; disable Bluetooth in `sdkconfig.defaults`; update SoftAP UI status to CAN link/protocol.

**Tech Stack:** ESP-IDF 5.2, ESP32-C6, FreeRTOS, existing SoftAP HTTP + `can_obd`.

**Spec:** `docs/superpowers/specs/2026-07-31-ble-elm-hard-delete-design.md`

## Global Constraints

- SoftAP web UI stays; do not remove Wi-Fi
- OTA not in this plan
- Trafyn schema keys may keep `ble_connected`/`elm_ready` mirroring CAN (compat)
- Prefer delete over `#ifdef` feature flags
- Verify with `idf.py build` + `idf.py size` + host `ctest`

## File map

| Path | Change |
|------|--------|
| `components/ble_elm/**` | Delete |
| `components/elm327_client/**` | Delete |
| `components/elm_transport/**` | Delete |
| `main/app_main.c`, `main/CMakeLists.txt` | Drop BLE/ELM init + REQUIRES |
| `main/Kconfig.projbuild` | Remove `ELM_BLE_SCAN_MS`; rename menu optionally |
| `sdkconfig.defaults` | `CONFIG_BT_ENABLED` off / NimBLE off |
| `components/transport_serial/*` | CAN-only console |
| `components/transport_http/*` | Drop BLE APIs; CAN status; UI |
| `PROJECT_CONTEXT.md`, `README.md` | Reflect MCP2515 product |

---

### Task 1: Disable Bluetooth in defaults and drop BLE Kconfig

**Files:**
- Modify: `sdkconfig.defaults`
- Modify: `main/Kconfig.projbuild`
- Modify: `PROJECT_CONTEXT.md` (brief note)

**Interfaces:**
- Produces: BT disabled at config level so later unlink does not pull NimBLE

- [ ] **Step 1: Edit `sdkconfig.defaults`**

Remove or comment out:

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
CONFIG_ELM_BLE_SCAN_MS=12000
```

Add:

```
# CONFIG_BT_ENABLED is not set
```

Keep SoftAP/console/LTE settings unchanged.

- [ ] **Step 2: Edit `main/Kconfig.projbuild`**

Remove `ELM_BLE_SCAN_MS` entirely. Optionally rename menu from `"ELM327 Bridge"` to `"Fleet telematics node"`. Keep SoftAP SSID/pass and rename `ELM_CMD_TIMEOUT_MS` help text to OBD command timeout (still used by poller via CONFIG name — if poller still references `CONFIG_ELM_CMD_TIMEOUT_MS`, keep the symbol name to avoid a large rename).

- [ ] **Step 3: Commit**

```bash
git add sdkconfig.defaults main/Kconfig.projbuild PROJECT_CONTEXT.md
git commit -m "build: disable Bluetooth/NimBLE in defaults for CAN-only node"
```

---

### Task 2: Rewire `app_main` and remove BLE components from CMake

**Files:**
- Modify: `main/app_main.c`
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Consumes: `can_obd_init`, `can_obd_start`, `can_obd_is_ready`, `can_obd_get_protocol`
- Produces: Boot without `ble_elm` / `elm327_client`

- [ ] **Step 1: Update `main/CMakeLists.txt`**

```cmake
idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash sys_runtime profile_store telemetry_bus
                             can_obd obd_poller transport_serial transport_http
                             net_lte telemetry_uplink)
```

- [ ] **Step 2: Update `main/app_main.c`**

Remove `#include "ble_elm.h"` and `#include "elm327_client.h"`.

Remove the block:

```c
ESP_ERROR_CHECK(ble_elm_init());
...
ESP_ERROR_CHECK(elm327_client_init());
```

Keep `can_obd_init` / `can_obd_start` / `can_boot_task` / poller / transports / LTE / uplink.

- [ ] **Step 3: Commit**

```bash
git add main/app_main.c main/CMakeLists.txt
git commit -m "refactor: boot CAN path only; drop ble_elm and elm327_client from main"
```

---

### Task 3: Rewire `transport_serial` to CAN

**Files:**
- Modify: `components/transport_serial/transport_serial.c`
- Modify: `components/transport_serial/CMakeLists.txt`
- Modify: `components/transport_serial/include/transport_serial.h`

**Interfaces:**
- Consumes: `can_obd_is_ready`, `can_obd_transact`, `can_obd_get_protocol`, `cmd_policy_check`, `obd_poller_submit_raw` (or direct can_obd after policy)
- Produces: Console without scan/select/unbond

- [ ] **Step 1: Update CMakeLists**

```cmake
idf_component_register(SRCS "transport_serial.c"
                    INCLUDE_DIRS "include"
                    REQUIRES can_obd cmd_policy obd_poller profile_store telemetry_bus
                             sys_runtime net_lte telemetry_uplink freertos driver vfs)
```

- [ ] **Step 2: Replace includes and help text**

Use `can_obd.h`, `cmd_policy.h`. Remove `ble_elm.h`, `elm327_client.h`.

Help commands list (no scan/devices/select/unbond/init):

```
help, status, cmd, profiles, profile, telemetry, unsafe, metrics, lte, uplink
```

- [ ] **Step 3: Rewrite `cmd_status`**

Print:

```
can_ready=%s protocol=%s poller=%s profile=%s items=%d
```

using `can_obd_is_ready()`, `can_obd_get_protocol()`, `obd_poller_is_enabled()`, active profile.

- [ ] **Step 4: Rewrite `cmd` path**

For `cmd <OBD>`: if command starts with `AT`/`at`, print not supported. Else `cmd_policy_check` then `can_obd_transact` (or `obd_poller_submit_raw` if that already applies policy — prefer one path; poller already has policy — use `obd_poller_submit_raw` for consistency).

Remove all functions and dispatch arms for `scan`, `devices`, `select`, `unbond`, BLE `init`, and wait-for-`elm327_client_is_ready` helpers.

- [ ] **Step 5: Update header comments** in `transport_serial.h` to describe CAN + SoftAP console.

- [ ] **Step 6: Commit**

```bash
git add components/transport_serial
git commit -m "refactor: serial console uses can_obd; remove BLE scan/select commands"
```

---

### Task 4: Rewire `transport_http` SoftAP API + UI

**Files:**
- Modify: `components/transport_http/CMakeLists.txt`
- Modify: `components/transport_http/http_api.c`
- Modify: `components/transport_http/static_index.html.h` (and source HTML if generated from a non-header file — edit the embedded string)

**Interfaces:**
- Consumes: `can_obd_is_ready`, `can_obd_get_protocol`, `obd_poller_*`, uplink APIs
- Produces: Status JSON without BLE device APIs

- [ ] **Step 1: CMake** — drop `ble_elm`, `elm327_client`; add `can_obd` if missing.

- [ ] **Step 2: Status handlers** — replace `ble_elm_is_connected` / `elm327_client_is_ready` with CAN equivalents. Keep JSON keys `ble_connected`/`elm_ready` as aliases of CAN ready for schema/UI compat **or** rename UI fields to `can_ready` in the same change (prefer updating UI to `can_ready` + `obd_protocol`).

- [ ] **Step 3: Delete handlers** for BLE scan, device list, connect, disconnect, bond. Unregister routes. Any `init` that called `elm327_client_run_init_sequence` → no-op success or remove (CAN has no AT init).

- [ ] **Step 4: Raw OBD / profile routes** — gate on `can_obd_is_ready()`; execute via `obd_poller_submit_raw` or `can_obd_transact` + policy.

- [ ] **Step 5: SoftAP UI** — remove BLE device picker / scan buttons; show CAN link + protocol; keep telemetry, profiles, uplink cards.

- [ ] **Step 6: Commit**

```bash
git add components/transport_http
git commit -m "refactor: SoftAP API/UI use CAN link; remove BLE device endpoints"
```

---

### Task 5: Delete BLE/ELM components and fix remaining references

**Files:**
- Delete: `components/ble_elm/`
- Delete: `components/elm327_client/`
- Delete: `components/elm_transport/`
- Modify: any leftover `#include` / CMake REQUIRES / docs (`README.md`, `PROJECT_CONTEXT.md`)

**Interfaces:**
- Produces: Tree with no BLE ELM components

- [ ] **Step 1: Grep** for `ble_elm`, `elm327_client`, `elm_transport`, `nimble`, `ble_gap` under the repo (exclude docs/history). Fix all hits.

- [ ] **Step 2: Delete the three component directories**

```bash
rm -rf components/ble_elm components/elm327_client components/elm_transport
```

- [ ] **Step 3: Update README / PROJECT_CONTEXT** — product description MCP2515 + SoftAP + LTE; remove BLE ELM as primary path; note deleted components.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "chore: delete ble_elm, elm327_client, and elm_transport components"
```

---

### Task 6: Full clean build, size check, host tests

**Files:** none (verification)

- [ ] **Step 1: Host tests**

```bash
cd tests/host && cmake -B build && cmake --build build && (cd build && ctest --output-on-failure)
```

Expected: 4/4 PASS

- [ ] **Step 2: Clean rebuild**

```bash
cd <repo>
idf.py fullclean
idf.py set-target esp32c6
idf.py build
```

Expected: build succeeds; no link errors for ble_elm/NimBLE.

- [ ] **Step 3: Size**

```bash
python $IDF_PATH/tools/idf_size.py --archives build/elm327_esp32c6.map | head -40
```

Expected: no `libble_app.a` / `libbt.a` in top archives; total image **below previous ~1.27 MiB** by roughly 150–280 KiB.

- [ ] **Step 4: Optional device smoke** (if board connected)

Flash; confirm SoftAP + `status` shows can_ready; `cmd 010C` works with OBD cable; uplink still enabled.

- [ ] **Step 5: Final commit** only if docs/size notes needed; otherwise done.

```bash
git commit -m "docs: note flash size after BLE removal"  # if updating PROJECT_CONTEXT with new size
```

---

## Spec coverage checklist

| Spec item | Task |
|-----------|------|
| Delete ble_elm / elm327_client / elm_transport | 5 |
| Disable BT in sdkconfig.defaults | 1 |
| app_main CAN-only boot | 2 |
| Serial CAN commands | 3 |
| HTTP/UI CAN status, drop BLE APIs | 4 |
| Docs update | 1, 5 |
| Build + size + host tests | 6 |

## Out of scope (next plan)

- A/B OTA partitions + LTE firmware download
