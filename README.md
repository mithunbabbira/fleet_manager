# ESP32-C6 Fleet Telematics Node

ESP-IDF firmware that reads OBD-II directly through an **MCP2515 CAN controller**,
exposes configuration over **USB serial** (plus an optional PC **Carrier Console**),
and uploads telemetry through a **Quectel EC200U LTE modem**. Vehicle commands
pass through a read-only safety policy.

Pin map and printed-PCB wiring:
[`hardware/fleet_telematics_carrier/README.md`](hardware/fleet_telematics_carrier/README.md).

Design notes live under [`docs/superpowers/`](docs/superpowers/). Early BLE/ELM327
docs are historical only ([`docs/archive/`](docs/archive/)).

## Hardware requirements

Use the **printed carrier PCB** (`Vehical_Telematics_Design.zip`). Do not
recreate the old jumper harness or fab the retired Python Gerber draft.

| Item | Notes |
|---|---|
| **ESP32-C6 Super Mini** | **4 MB flash**. Dual-bank OTA: `ota_0` + `ota_1`. |
| **MCP2515 + TXS0108E** | Soft-SPI GPIO21 SCK, 22 MOSI, 23 MISO, 20 CS, 14 INT (3.3 V ↔ 5 V). |
| **microSD** | Dedicated SPI2: GPIO4 SCK, 5 MOSI, 6 MISO, 18 CS. |
| **Quectel EC200U** | UART1 **GPIO16 TX → modem RX**, **GPIO17 RX ← modem TX**, 115200 8N1. Separate VBAT + common GND. |

Full TXS channel table and LTE UART notes are in the hardware README. Firmware
already matches the printed LTE copper; a PCB respin is not required for RX/TX.

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

## Carrier Console (PC web UI)

On-device Wi-Fi/HTTP is **disabled by default** (saves flash; avoids Zigbee contention).
Configure the carrier from your computer over USB:

```bash
python3 -m pip install -r tools/carrier_console/requirements.txt
python3 tools/carrier_console/app.py --port /dev/cu.usbmodem1101
```

Opens **http://127.0.0.1:8766** — uplink, OTA, LTE, profiles, OBD/DTC/VIN, fleet hosts.
See [`tools/carrier_console/README.md`](tools/carrier_console/README.md).

Legacy on-device SoftAP/HTTP can be re-enabled with `CONFIG_ELM_HTTP_ENABLE=y` in menuconfig.

## Serial console commands

Connect over USB serial (115200 8N1). Type `help` for the built-in list.

| Command | Description |
|---|---|
| `help` | List available commands |
| `config` | Key=value dump for Carrier Console |
| `status` | CAN readiness/protocol, active profile, poller state, and metrics |
| `cmd <OBD>` | Send a raw OBD command through the safety gate |
| `vin` | Read VIN (Mode 09 PID 02) |
| `dtc read\|clear` | Read stored/pending DTCs or clear (Mode 04, needs `unsafe on`) |
| `profiles` | List stored profiles (`*` marks active) |
| `profile <name>` | Switch active profile and reload the poller |
| `telemetry on\|off` | Stream live telemetry samples to the console |
| `unsafe on\|off` | Allow/deny unsafe (write-capable) OBD commands |
| `metrics` | Print runtime counters as JSON |
| `lte [reconnect\|test]` | Show LTE status, reconnect, or run the modem self-test |
| `uplink [on\|off\|now\|qtest\|device_id\|node_id\|interval]` | Cloud uplink config and triggers |
| `ota [status\|run\|force\|url\|device_id]` | LTE firmware OTA |
| `fleet hosts` | Zigbee host registry snapshot (when `CONFIG_FLEET_ZIGBEE_ENABLE`) |
| `fleet ingest <hex>` | Loopback TLV ingest test (no RF) |

Built-in profiles: `fleet_basic` (default polling set) and `diagnostics` (adds throttle, DTC, VIN).

## Fleet Zigbee hosts (optional)

A second ESP32-C6 can join as a Zigbee end device and stream sensor TLV frames to the
carrier coordinator. The network is **open** (lab use) on channel 15 — no install codes.

| Doc | Purpose |
|---|---|
| [`docs/fleet-zigbee-host-guide.md`](docs/fleet-zigbee-host-guide.md) | Add hosts, manifests, build flags |
| [`docs/fleet-zigbee-coexistence.md`](docs/fleet-zigbee-coexistence.md) | BLE / Wi-Fi / Zigbee RF planning |

Reference host firmware: `hardware/fleet_telematics_carrier/host/ul212-ble-fetch/` (UL212 BLE → Zigbee).

## LTE transport

Uplink and firmware OTA use **Quectel QHTTP AT commands** over UART — not PPP or the
ESP32 TCP/IP stack. The modem self-test reports `HTTP via QHTTP (no PPP)` when UART
bring-up succeeds.

## Safety policy

All OBD traffic passes through `cmd_policy` before reaching the CAN bus:

- **Allowlist only** — only explicitly permitted AT commands and OBD modes are forwarded.
- **Mode 04 (clear DTCs) blocked** by default. `unsafe on` lifts this restriction for Mode 04 only.
- **Mode 08 (control/on-board systems) never allowed**, even with `unsafe on`.
- **Allowed OBD modes (read-only):** 01, 02, 03, 07, 09, 0A.
- **Profiles are validated read-only** — `profile_store_upsert()` rejects any init or poll command that fails the policy gate.

Blocked commands return an error on serial; the vehicle never receives them.

## Firmware OTA

Over LTE, the device POSTs to Trafyn `get-latest-device-firmware` to check for
updates and streams the presigned `.bin` URL when a newer version is available.
Configure `device_id`, manifest URL, and force via serial or Carrier Console. See
[`docs/superpowers/specs/2026-08-19-trafyn-firmware-ota-design.md`](docs/superpowers/specs/2026-08-19-trafyn-firmware-ota-design.md).

## Host tests

Pure-logic components have host-side unit tests that run without hardware (8 tests):

```bash
cmake -S tests/host -B tests/host/build
cmake --build tests/host/build
ctest --test-dir tests/host/build --output-on-failure
```

Covers `cmd_policy`, `obd_codec`, `obd_isotp`, `uplink_payload`, `fw_ota_lte_parse`,
`fleet_tlv`, `host_registry`, and `fleet_uplink_path`.

## Architecture

Layered ESP-IDF components keep vehicle I/O independent from local and cellular
transports.

```
app_main
  → sys_runtime          (WDT, logs, metrics, OTA stub)
  → profile_store        (NVS profiles and safety flags)
  → telemetry_bus        (typed pub/sub)
  → transport_serial     (USB console — primary config path)
  → transport_http       (optional SoftAP + REST; default off)
  → transport_zigbee     (optional Zigbee coordinator + host ingest)
  → host_registry        (dynamic Zigbee host catalog)
  → net_lte              (EC200U modem, QHTTP AT)
  → telemetry_uplink     (HTTPS fleet telemetry)
  → obd_poller           (profile-driven polling)
  → cmd_policy           (read-only safety gate)
  → can_obd              (MCP2515 + ISO-TP + protocol detection)
  → obd_codec            (PID decode, DTC/VIN helpers)
```

The BLE ELM327 adapter stack was deleted; the MCP2515 is the only vehicle data
path.

## On-device bring-up checklist

Lab (printed PCB, 2026-08-18): MCP detect, SD mount, and LTE AT/Airtel
registration passed. CAN ECU replies still need a vehicle on the bus.

- [ ] Carrier Console connects over USB; `config` populates uplink/OTA fields
- [ ] Serial `status` reports `can_ready=yes` and the detected CAN protocol
- [ ] `cmd 010C` returns engine RPM data
- [ ] `cmd 04` is **blocked** (policy error); vehicle never receives it
- [ ] `cmd 08` is **blocked** even after `unsafe on`
- [ ] Profile switch works (e.g. `profile diagnostics`)
- [ ] Disconnect/reconnect CAN → protocol detection recovers and polling resumes
- [ ] LTE `test` succeeds (`HTTP via QHTTP`) and `uplink now` enqueues to SD (`sd=yes`)
- [ ] With Zigbee enabled: `fleet hosts` shows joined host readings (~1 Hz)
- [ ] `metrics` counters increment (`cmds_ok`, `cmds_fail`, `blocked_cmds`, etc.) during normal use
