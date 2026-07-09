# ELM327 ESP32-C6 Bridge

ESP-IDF firmware for the **ESP32-C6 Mini** that connects to a **BLE ELM327 Mini** OBD-II adapter and exposes vehicle data through a read-only safety gate. v1 ships with **USB serial console** and **SoftAP HTTP** transports; fleet UART and Zigbee are reserved for later.

Design and implementation notes live under [`docs/superpowers/`](docs/superpowers/):

- [Design spec](docs/superpowers/specs/2026-07-09-elm327-esp32c6-design.md)
- [Implementation plan](docs/superpowers/plans/2026-07-09-elm327-esp32c6-implementation.md)

## Hardware requirements

| Item | Notes |
|---|---|
| **ESP32-C6 Mini** | Must have **8 MB flash**. The partition table and `sdkconfig.defaults` assume 8 MB (`factory` + dual OTA slots). |
| **BLE ELM327 Mini** | **Bluetooth Low Energy only.** The ESP32-C6 has no Bluetooth Classic radio — Classic-only adapters will not work. |

### Verify BLE vs Bluetooth Classic

Most phone OBD apps expect Bluetooth Classic (SPP). Use a BLE scanner instead:

1. Install **nRF Connect** (iOS/Android) or a similar BLE scanner.
2. Power the ELM327 adapter and scan for nearby devices.
3. A **BLE** adapter advertises as a connectable LE device and exposes GATT services (often Nordic UART Service, NUS).
4. A **Classic-only** adapter may appear in the phone’s Bluetooth paired-device list but **will not** show up as a BLE peripheral in nRF Connect.

If your adapter does not expose NUS (`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`), capture its service and characteristic UUIDs with nRF Connect and configure overrides (see [UUID overrides](#uuid-overrides) below).

## Build and flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) (v5.x) with the ESP32-C6 tool chain installed.

```bash
source ~/esp/esp-idf/export.sh
cd <repo>
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with your serial device (e.g. `/dev/cu.usbmodem101` on macOS, `/dev/ttyACM0` on Linux).

## SoftAP defaults

On boot the device starts a Wi-Fi SoftAP for the web UI and REST API:

| Setting | Default |
|---|---|
| SSID | `ELM327-C6` |
| Password | `elm327c6` |
| Web UI | [http://192.168.4.1/](http://192.168.4.1/) |

Values are configurable via Kconfig (`main/Kconfig.projbuild`) or menuconfig.

## Serial console commands

Connect over USB serial (115200 8N1). Type `help` for the built-in list.

| Command | Description |
|---|---|
| `help` | List available commands |
| `status` | BLE connected, ELM ready, active profile, key metrics |
| `scan` | Scan for BLE ELM327 adapters (~configurable duration) |
| `devices` | Print results from the last scan (index, name, addr, RSSI) |
| `select <idx>` | Bond to device `<idx>` from the last scan and connect |
| `cmd <AT/OBD>` | Send a raw AT or OBD command through the safety gate |
| `profiles` | List stored profiles (`*` marks active) |
| `profile <name>` | Switch active profile and reload the poller |
| `telemetry on\|off` | Stream live telemetry samples to the console |
| `unsafe on\|off` | Allow/deny unsafe (write-capable) OBD commands |
| `metrics` | Print runtime counters as JSON |

Built-in profiles: `fleet_basic` (default polling set) and `diagnostics` (adds throttle, DTC, VIN).

## Safety policy

All AT and OBD traffic passes through `cmd_policy` before bytes reach the ELM327:

- **Allowlist only** — only explicitly permitted AT commands and OBD modes are forwarded.
- **Mode 04 (clear DTCs) blocked** by default. `unsafe on` lifts this restriction for Mode 04 only.
- **Mode 08 (control/on-board systems) never allowed**, even with `unsafe on`.
- **Allowed OBD modes (read-only):** 01, 02, 03, 07, 09, 0A.
- **Profiles are validated read-only** — `profile_store_upsert()` rejects any init or poll command that fails the policy gate.

Blocked commands return an error on serial and HTTP 403 on the web API; the adapter never receives the bytes.

## UUID overrides

By default the firmware discovers **Nordic UART Service (NUS)** GATT UUIDs:

- Service: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- RX (write): `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- TX (notify): `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`

Clone adapters may use different UUIDs. Capture them with nRF Connect (connect → browse services → note 128-bit UUIDs for the UART-like write and notify characteristics).

### Bond record (NVS)

BLE pairing state is stored in NVS namespace `elm`, key `bond`, as JSON via `profile_store`. The `ble_bond_t` structure includes:

| Field | Purpose |
|---|---|
| `addr` / `addr_set` | BLE MAC address of the selected adapter |
| `name` | Human-readable device name from scan |
| `service_uuid` | GATT service UUID override (dashed 128-bit string) |
| `rx_uuid` | Write characteristic UUID override |
| `tx_uuid` | Notify characteristic UUID override |

**What `select` persists:** the serial and HTTP `select` flows save **address and name only**. They do not capture UUID overrides.

**Applying UUID overrides:** after identifying the correct UUIDs, write all three UUID strings into the bond JSON (alongside the saved `addr`). On next boot or reconnect, `ble_elm` reads the bond from `profile_store` and uses the overrides when `service_uuid`, `rx_uuid`, and `tx_uuid` are all non-empty; otherwise NUS defaults apply.

Example bond JSON shape (stored under NVS key `bond`):

```json
{
  "addr": "AA:BB:CC:DD:EE:FF",
  "name": "OBDII",
  "service_uuid": "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX",
  "rx_uuid": "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX",
  "tx_uuid": "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"
}
```

There is no dedicated serial command for UUID entry today; use NVS tooling or extend `profile_store_set_bond()` via a future API if needed.

## Host tests

Pure-logic components (`cmd_policy`, `obd_codec`) have host-side unit tests that run without hardware:

```bash
mkdir -p tests/host/build && cd tests/host/build
cmake .. && cmake --build . && ctest --output-on-failure
```

## Architecture

Layered ESP-IDF components. OBD/ELM logic does not depend on HTTP, Zigbee, or fleet UART.

```
app_main
  → sys_runtime          (WDT, logs, metrics, OTA stub)
  → profile_store        (NVS profiles, bond, safety flags)
  → telemetry_bus        (typed pub/sub)
  → transport_serial     (USB console)
  → transport_http       (SoftAP + REST + web UI)
  → obd_poller           (profile-driven polling)
  → cmd_policy           (read-only safety gate)
  → elm327_client        (AT session, timeouts)
  → elm_transport        (byte I/O interface)
       ↳ ble_elm         (NimBLE central — scan, connect, GATT)
  → obd_codec            (PID decode, DTC/VIN helpers)
```

**Future transports** (stubs/interfaces only in v1): fleet-management UART bridge and Zigbee peer — both will subscribe to `telemetry_bus` and reuse the same command APIs without talking to BLE directly.

## On-device bring-up checklist

> **For the operator:** complete this checklist on real hardware after flashing. On-device validation has **not** been performed as part of firmware development in CI — tick each item in your PR or lab notes.

- [ ] SoftAP **`ELM327-C6`** appears; join with password **`elm327c6`**; open [http://192.168.4.1/](http://192.168.4.1/)
- [ ] Serial `scan` or web scan lists the ELM327 adapter
- [ ] `select <idx>` (or web equivalent); observe **`init_ok`** event (serial telemetry or HTTP status)
- [ ] `cmd 010C` returns engine RPM data
- [ ] `cmd 04` is **blocked** (policy error / HTTP 403); adapter never receives it
- [ ] `cmd 08` is **blocked** even after `unsafe on`
- [ ] Profile switch works (e.g. `profile diagnostics`)
- [ ] Power-cycle the ELM327 adapter → ESP32-C6 **auto-reconnects** to the bonded address
- [ ] `metrics` counters increment (`cmds_ok`, `cmds_fail`, `blocked_cmds`, etc.) during normal use
