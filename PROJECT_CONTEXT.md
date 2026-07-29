# Fleet Manager (ELM327 ESP32-C6) — Project Context & Handoff

> Purpose: give any engineer or LLM enough context to continue this project without
> re-reading the whole tree. Read this first, then dive into the files it references.
>
> Last updated: 2026-07-23. Keep this file current when architecture or WIP changes.

---

## 1. What this project is

ESP-IDF firmware (CMake project name **`elm327_esp32c6`**) for an **ESP32-C6 Mini**.
It is a bridge/telematics node that:

- Connects as a **BLE central (NimBLE)** to a **BLE-only ELM327 Mini** OBD-II adapter.
- Runs **profile-driven OBD polling** and decodes PIDs / DTCs / VIN via `obd_codec`.
- Enforces a **read-only safety gate** (`cmd_policy`) on every AT/OBD command.
- Exposes control + telemetry over **USB Serial/JTAG console** and a **Wi-Fi SoftAP
  + HTTP REST API + embedded web UI**.
- (WIP) Adds a **Quectel EC200U LTE** modem over UART for future cellular uplink.

The repo lives under `fleetHub`; product direction is **fleet telematics** — collect
vehicle data on-device and eventually uplink to a fleet backend. The default OBD
profile is named `fleet_basic`.

### Design docs (read for rationale)
- `docs/superpowers/specs/2026-07-09-elm327-esp32c6-design.md` — architecture/spec.
- `docs/superpowers/plans/2026-07-09-elm327-esp32c6-implementation.md` — implementation plan.
- `docs/sample-obd-telemetry.md` — sample telemetry shape for uplink design.
- `README.md` — user-facing build/flash/usage (note: **not yet updated** for LTE).

---

## 2. Hardware

| Item | Detail |
|------|--------|
| MCU | ESP32-C6 Mini, 4 MB flash (e.g. ESP32-C6FH4) |
| OBD adapter | BLE ELM327 Mini (NimBLE central; **no Bluetooth Classic**) |
| Console | USB Serial/JTAG, 115200 8N1 |
| Wi-Fi | SoftAP, default SSID `ELM327-C6`, password `elm327c6`, UI at `http://192.168.4.1/` |
| LTE modem | Quectel **EC200U** |
| Modem UART | **UART1**: ESP **GPIO17 = TX → modem RX**, **GPIO16 = RX ← modem TX**, 115200 8N1, common GND. Modem needs its own power + PWRKEY boot. |
| Default APN | `airtelgprs.com` (Airtel; confirmed via host probe) |
| Enclosure | `case_design/truck_dashboard_case.scad` — case for ESP32-C6 + EC200U + XY-3606 power module, ~122×82 mm base |

> UART1 conflict note: a documented (not-yet-built) future "fleet synthetic UART"
> transmitter also targets GPIO16/17. Only one owner of UART1 at a time.

---

## 3. Build / flash / test

```bash
source ~/esp/esp-idf/export.sh          # ESP-IDF v5.x
cd /home/mithun/Documents/projets/fleetHub/fleet_manager
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor    # USB Serial/JTAG
```

Host unit tests (pure-C logic):
```bash
cd tests/host && cmake -B build && cmake --build build && (cd build && ctest)
```
Covers `cmd_policy` (`test_cmd_policy.c`) and `obd_codec` (`test_obd_codec.c`).

Key `sdkconfig.defaults` values:
- `CONFIG_IDF_TARGET="esp32c6"`, 4 MB flash, custom `partitions.csv`.
- `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (no UART0 console).
- NimBLE central+observer, SoftAP support, Task WDT 10 s, main stack 8192.
- `CONFIG_ELM_CMD_TIMEOUT_MS=12000`, `CONFIG_ELM_BLE_SCAN_MS=12000`.
- `CONFIG_NET_LTE_ENABLE=y`, `CONFIG_NET_LTE_APN="airtelgprs.com"`.

Kconfig menus: `main/Kconfig.projbuild` (SoftAP creds, ELM timeouts, BLE scan) and
`components/net_lte/Kconfig` (UART port/pins/baud, APN).

Partitions (`partitions.csv`): `nvs`, `otadata`, `phy_init`, `factory` @0x20000
(0x1C0000), `ota_0` @0x1E0000 (0x1C0000).

---

## 4. Directory layout

```
fleet_manager/
├── CMakeLists.txt              # project(elm327_esp32c6)
├── README.md
├── partitions.csv
├── sdkconfig.defaults
├── case_design/truck_dashboard_case.scad
├── components/
│   ├── elm_transport/          # header-only byte-I/O transport interface
│   ├── ble_elm/                # NimBLE central: scan/connect/GATT/reconnect
│   ├── elm327_client/          # single-flight AT/OBD session over transport
│   ├── cmd_policy/             # read-only allowlist gate (pure C, unit-tested)
│   ├── obd_codec/              # PID/DTC/VIN decoders (pure C, unit-tested)
│   ├── profile_store/          # NVS profiles, active profile, BLE bond, allow_unsafe
│   ├── obd_poller/             # FreeRTOS task: scheduled polling + raw cmd queue
│   ├── telemetry_bus/          # in-proc pub/sub fan-out to subscribers
│   ├── sys_runtime/            # WDT heartbeat, metrics counters, OTA stub
│   ├── transport_serial/       # USB Serial/JTAG interactive console
│   ├── transport_http/         # SoftAP + httpd REST API + embedded web UI
│   └── net_lte/                # NEW/WIP: Quectel EC200U LTE over UART1
├── docs/…                      # specs, plans, sample telemetry
├── main/                       # app_main.c, CMakeLists.txt, Kconfig.projbuild
├── tests/host/                 # host cmake/ctest unit tests
└── tools/ec200u_at_probe.py    # host-side EC200U AT probe
```

---

## 5. Architecture & data flow

Layered, decoupled by the transport interface and telemetry bus:

```
BLE ELM327  ──ble_elm──►  elm_transport_t  ──►  elm327_client (AT/OBD session)
                                                     ▲   (all commands pass cmd_policy first)
                                                     │
   profile_store (NVS profiles) ──► obd_poller ──────┘
                                        │ publishes samples/errors
                                        ▼
                                  telemetry_bus  ──► subscribers:
                                                     • transport_http (/api/telemetry cache)
                                                     • transport_serial (telemetry dump)
sys_runtime: metrics + WDT heartbeat across the whole system
net_lte: independent UART1 modem status (not yet wired into telemetry/uplink)
```

Key rule: **`elm327_client` does NOT enforce policy**. Callers (`obd_poller`,
transports) must run `cmd_policy_check` before submitting raw commands.

### Component API cheat-sheet

- **`elm_transport`** (`elm_transport.h`): struct with `write`, `read_line`,
  `is_ready` fn pointers + `ctx`. The seam between BLE and the ELM client.
- **`ble_elm`** (`ble_elm.h`): `ble_elm_init`, `ble_elm_start_scan`,
  `ble_elm_get_scan_results`, `ble_elm_connect_addr`, `ble_elm_disconnect`,
  `ble_elm_is_connected`, `ble_elm_get_peer_addr`, `ble_elm_get_transport`,
  `ble_elm_start_auto_reconnect`/`stop`/`clear_peer`. Supports NUS + FFF0/FFE0
  fallback profiles; auto-reconnect with 1–30 s exponential backoff. UUID overrides
  read from NVS bond JSON.
- **`elm327_client`** (`elm327_client.h`): `..._init`, `..._set_transport`,
  `..._run_init_sequence`, `..._transact(cmd, resp, len, timeout_ms)`, `..._is_ready`.
  Mutex single-flight; ATZ ≥5 s timeout; classifies NO DATA/ERROR/SEARCHING.
- **`cmd_policy`** (`cmd_policy.h`): `..._normalize`, `..._check`, `..._is_allowed`,
  `..._result_str`. OBD modes 01/02/03/07/09/0A allowed; 04 requires `allow_unsafe`;
  08 always denied; unknown denied. AT allowlist (ATZ, ATSP0-9, ATRV, ATDP…).
- **`obd_codec`** (`obd_codec.h`): `..._decode_mode01`, `..._parse_dtcs`,
  `..._parse_vin`, `..._decode_named` (named decoders: rpm, speed, coolant_c, …).
- **`profile_store`** (`profile_store.h`): NVS namespace `elm`. `..._init`,
  `get/set_active`, `list`, `upsert` (policy-validated), `get/set_bond`,
  `get/set_allow_unsafe`. Built-ins in `builtin_profiles.h`: `fleet_basic`,
  `can_11_500`, `diagnostics`. Types: `obd_profile_t`, `profile_item_t`, `ble_bond_t`.
- **`obd_poller`** (`obd_poller.h`): `..._start`/`stop`, `..._reload_active_profile`,
  `..._set_enabled`/`is_enabled`, `..._submit_raw(cmd, resp, len, timeout_ms)`
  (priority queue depth 2 for serial/HTTP raw commands).
- **`telemetry_bus`** (`telemetry_bus.h`): `..._init`, `..._subscribe(QueueHandle_t*,
  filter_mask)`, `..._publish`. ≤4 subscribers, queue depth 16, non-blocking;
  bumps `telemetry_drops` metric on full queues. Msg types: `TELEMETRY_PID_SAMPLE`,
  `DTC_LIST`, `ELM_EVENT`, `ERROR`.
- **`sys_runtime`** (`sys_runtime.h`): `..._init`, `..._metric_inc`/`get`,
  `..._metrics_snapshot_json`, `..._ota_stub_status`. Metrics: `cmds_ok`, `cmds_fail`,
  `ble_reconnects`, `blocked_cmds`, `telemetry_drops`, `uptime_s`. OTA is a stub only.
- **`net_lte`** — see section 7.

---

## 6. Application boot flow (`main/app_main.c`)

1. `nvs_flash_init()` (erase + retry on NO_FREE_PAGES / NEW_VERSION).
2. `sys_runtime_init()` — WDT heartbeat + metrics.
3. `profile_store_init()`.
4. `telemetry_bus_init()`.
5. `net_lte_start()` — **non-fatal** on failure; boot continues without LTE.
6. `ble_elm_init()`.
7. `elm327_client_init()`.
8. `transport_serial_start()` — console task.
9. `transport_http_start()` — SoftAP + HTTP server.
10. **Bonded boot:** if NVS bond has `addr_set` → `ble_elm_connect_addr` →
    `elm327_client_set_transport(ble_elm_get_transport())` → spawn `bonded_boot_init_task`
    (waits ≤15 s for ELM ready, runs profile init AT + `0100`, enables poller; on
    timeout clears stale bond).
11. `obd_poller_start()`.
12. `ble_elm_start_auto_reconnect()` only if a bond exists **and** already connected
    (avoids blocking scan on a stale address).

No dedicated app event loop; work runs in FreeRTOS tasks. NimBLE/Wi-Fi use the
IDF default event loop.

### Task inventory

| Task | Component | Stack | Prio |
|------|-----------|-------|------|
| heartbeat | sys_runtime | 3072 | idle+1 |
| lte_bringup | net_lte | 4096 | 4 |
| console | transport_serial | 6144 | 4 |
| telem_dump (opt) | transport_serial | 3072 | 3 |
| http_tele_cache | transport_http | 3072 | 3 |
| obd_poller | obd_poller | 4096 | 5 |
| bonded_boot | app_main | 4096 | 5 |
| ble_reconnect | ble_elm | 4096 | 5 |

---

## 7. Active WIP: LTE + cloud uplink

**LTE status:** UART AT bring-up + modem self-test + **HTTPS POST via Quectel QHTTP*** (no PPP/`esp_netif` yet).

**Cloud uplink (`telemetry_uplink`):** SoftAP “Cloud uplink” card / `/api/uplink*`. Default **disabled**. When enabled, posts OBD snapshots to `https://api.trafyn.info/nc-events-api/v2/messages` (`schemaId` 1087) over LTE on the NVS interval (default 5 s). Gated on live BLE+ELM+poller+fresh sample. Missing/stale PIDs are JSON `null` + `*_ok:false`.

Files: `components/net_lte/*`, `components/telemetry_uplink/*`, SoftAP UI/API wiring.

---

## 8. Interfaces

### Serial console (transport_serial, USB Serial/JTAG @115200)
Interactive line reader with echo. Commands include the README v1 set plus newer
additions: `init`, `unbond`, `lte`, `lte reconnect`, and `telemetry on`. (README's
command list is stale — reconcile with `transport_serial.c` for the authoritative list.)

### HTTP REST API (transport_http, SoftAP + httpd :80)
SoftAP WPA2, ch1, ≤4 STA. httpd stack 12288, recv/send timeout 45 s.

| Method | URIs |
|--------|------|
| GET | `/`, `/api/status`, `/api/ble/devices`, `/api/protocol`, `/api/profiles`, `/api/telemetry`, `/api/metrics`, `/api/lte`, `/api/vin`, `/api/safety`, `/api/health` |
| POST | `/api/ble/scan`, `/api/ble/select`, `/api/ble/disconnect`, `/api/elm/cmd`, `/api/elm/init`, `/api/protocol`, `/api/protocol/detect`, `/api/profiles/active`, `/api/lte/reconnect`, `/api/dtc/read`, `/api/dtc/clear`, `/api/safety` |
| PUT | `/api/profiles` |

Embedded web UI is compiled from `static_index.html.h` (LTE status card added).

---

## 9. Uncommitted work snapshot (as of handoff)

New (untracked): `components/net_lte/*`, `tools/ec200u_at_probe.py`,
`case_design/truck_dashboard_case.scad`, this file.

Modified: `sdkconfig.defaults` (LTE enable + APN), `main/{app_main.c,CMakeLists.txt}`
(net_lte_start + bonded boot task + dep), `components/transport_serial/*` (`lte`
command + dep + USB-JTAG line reader), `components/transport_http/*` (LTE REST + UI
card + dep + httpd stack/timeouts).

README has **not** been updated for LTE or the newer serial commands.

---

## 10. Suggested next steps

1. **LTE data path (biggest gap):** implement PPP over the EC200U (esp-modem or
   raw AT + `esp_netif` PPP), set `link_up`/`ip_up`/`ip[]`, then a telemetry uplink
   (MQTT or HTTP POST) that subscribes to `telemetry_bus`. Keep the layered design.
2. **Resolve UART1 ownership** vs the planned fleet synthetic-UART transmitter.
3. **Update README**: LTE wiring/APN section, current serial command list, `/api/lte*`.
4. **Serial command for GATT UUID overrides** (currently manual NVS JSON only).
5. **Finish OTA** (currently `sys_runtime` stub) if firmware updates are needed.
6. Run host tests before releases; add tests for new logic.
