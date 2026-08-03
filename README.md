# ESP32-C6 Fleet Telematics Node

ESP-IDF firmware that reads OBD-II directly through an **MCP2515 CAN controller**,
serves local diagnostics over a **Wi-Fi SoftAP** and USB console, and uploads
telemetry through a **Quectel EC200U LTE modem**. Vehicle commands pass through a
read-only safety policy.

Design and implementation notes live under [`docs/superpowers/`](docs/superpowers/):

- [Design spec](docs/superpowers/specs/2026-07-09-elm327-esp32c6-design.md)
- [Implementation plan](docs/superpowers/plans/2026-07-09-elm327-esp32c6-implementation.md)

## Hardware requirements

| Item | Notes |
|---|---|
| **ESP32-C6 Mini** | **4 MB flash** (ESP32-C6FH4 and similar). Partition table: `factory` + one `ota_0` slot. |
| **MCP2515 module** | SPI CAN controller connected to the vehicle OBD-II CAN-H/CAN-L lines; protocol detection covers 11/29-bit IDs at 500/250 kbit/s. |
| **Quectel EC200U** | LTE modem on UART1: GPIO17 TX, GPIO16 RX, 115200 8N1. Use a separate suitable supply and common ground. |

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
| SSID | `Fleet-C6` |
| Password | `fleetc6` |
| Web UI | [http://192.168.4.1/](http://192.168.4.1/) |

Values are configurable via Kconfig (`main/Kconfig.projbuild`) or menuconfig.

## Serial console commands

Connect over USB serial (115200 8N1). Type `help` for the built-in list.

| Command | Description |
|---|---|
| `help` | List available commands |
| `status` | CAN readiness/protocol, active profile, poller state, and metrics |
| `cmd <OBD>` | Send a raw OBD command through the safety gate |
| `profiles` | List stored profiles (`*` marks active) |
| `profile <name>` | Switch active profile and reload the poller |
| `telemetry on\|off` | Stream live telemetry samples to the console |
| `unsafe on\|off` | Allow/deny unsafe (write-capable) OBD commands |
| `metrics` | Print runtime counters as JSON |
| `lte [reconnect\|test]` | Show LTE status, reconnect, or run the modem self-test |
| `uplink [on\|off\|now]` | Configure or trigger the cloud uplink |

Built-in profiles: `fleet_basic` (default polling set) and `diagnostics` (adds throttle, DTC, VIN).

## Safety policy

All OBD traffic passes through `cmd_policy` before reaching the CAN bus:

- **Allowlist only** — only explicitly permitted AT commands and OBD modes are forwarded.
- **Mode 04 (clear DTCs) blocked** by default. `unsafe on` lifts this restriction for Mode 04 only.
- **Mode 08 (control/on-board systems) never allowed**, even with `unsafe on`.
- **Allowed OBD modes (read-only):** 01, 02, 03, 07, 09, 0A.
- **Profiles are validated read-only** — `profile_store_upsert()` rejects any init or poll command that fails the policy gate.

Blocked commands return an error on serial and HTTP 403 on the web API; the
vehicle never receives them.

## Host tests

Pure-logic components (`cmd_policy`, `obd_codec`) have host-side unit tests that run without hardware:

```bash
mkdir -p tests/host/build && cd tests/host/build
cmake .. && cmake --build . && ctest --output-on-failure
```

## Architecture

Layered ESP-IDF components keep vehicle I/O independent from local and cellular
transports.

```
app_main
  → sys_runtime          (WDT, logs, metrics, OTA stub)
  → profile_store        (NVS profiles and safety flags)
  → telemetry_bus        (typed pub/sub)
  → transport_serial     (USB console)
  → transport_http       (SoftAP + REST + web UI)
  → net_lte              (EC200U modem)
  → telemetry_uplink     (HTTPS fleet telemetry)
  → obd_poller           (profile-driven polling)
  → cmd_policy           (read-only safety gate)
  → can_obd              (MCP2515 + ISO-TP + protocol detection)
  → obd_codec            (PID decode, DTC/VIN helpers)
```

The legacy wireless adapter components were deleted; the MCP2515 is the only
vehicle data path.

## On-device bring-up checklist

> **For the operator:** complete this checklist on real hardware after flashing. On-device validation has **not** been performed as part of firmware development in CI — tick each item in your PR or lab notes.

- [ ] SoftAP **`Fleet-C6`** appears; join with password **`fleetc6`**; open [http://192.168.4.1/](http://192.168.4.1/)
- [ ] Serial `status` reports `can_ready=yes` and the detected CAN protocol
- [ ] `cmd 010C` returns engine RPM data
- [ ] `cmd 04` is **blocked** (policy error / HTTP 403); vehicle never receives it
- [ ] `cmd 08` is **blocked** even after `unsafe on`
- [ ] Profile switch works (e.g. `profile diagnostics`)
- [ ] Disconnect/reconnect CAN → protocol detection recovers and polling resumes
- [ ] LTE `test` succeeds and `uplink now` reports a successful HTTP response
- [ ] `metrics` counters increment (`cmds_ok`, `cmds_fail`, `blocked_cmds`, etc.) during normal use
