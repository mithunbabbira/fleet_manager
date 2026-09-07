# ESP32-C6 Fleet Telematics — Project Context

> Last updated: 2026-09-07 (Bitbucket main cleaned around `firmware_v2/master`).
>
> Production data path: vehicle CAN through MCP2515; optional Zigbee sensor hosts
> via `transport_zigbee` → `host_registry`. Local access: USB Serial/JTAG plus
> PC **Carrier Console** (`tools/carrier_console`, port 8766). Cloud: Quectel EC200U
> over **QHTTP AT** (no PPP). The BLE ELM327 adapter stack and the legacy root
> ESP-IDF app (`elm327_esp32c6`) have been removed from this tree.
>
> Authoritative pin map:
> `hardware/fleet_telematics_carrier/README.md`
> Fabricated gerbers: `Vehical_Telematics_Design.zip` (KiCad 9, 2026-08-12).
>
> Flash layout is dual-bank OTA (`ota_0` + `ota_1`). LTE firmware check: Trafyn
> POST `get-latest-device-firmware` (see
> `docs/superpowers/specs/2026-08-19-trafyn-firmware-ota-design.md`).
>
> Telemetry uplink: Trafyn
> `POST …/nc-events-api/v1/sources/nc-fleet-device/messages` (bare JSON array;
> master sets `device_type` for OBD/GPS only).

## Product

ESP-IDF firmware under **`firmware_v2/master/`** (CMake project name
`fleet_v2_master`) for an ESP32-C6 Super Mini. It:

- detects ISO 15765-4 CAN at 11/29-bit identifiers and 500/250 kbit/s;
- polls Mode-01 OBD PIDs and builds schema **1087** envelopes;
- posts modem GNSS as schema **1089**;
- ingests Zigbee host TLV reports as schema **1088** (dynamic hosts);
- exposes status and control through USB serial and the PC Carrier Console;
- posts telemetry through an EC200U using Quectel QHTTP AT commands (HTTPS);
- stores failed posts on microSD and drains when LTE recovers;
- updates itself via Trafyn get-latest dual-bank OTA.

## Hardware (printed PCB — what firmware uses)

| Item | Detail |
|---|---|
| MCU | ESP32-C6 Super Mini, 4 MB flash |
| CAN | MCP2515 over **soft-SPI** through TXS0108E; GPIO21 SCK, 22 MOSI, 23 MISO, 20 CS, 14 INT |
| microSD | Hardware SPI2 GPIO4 SCK, 5 MOSI, 6 MISO, 18 CS |
| Console | USB Serial/JTAG, 115200 8N1; PC UI at `tools/carrier_console` |
| LTE | EC200U UART1: **GPIO16 ESP-TX → modem RX**, **GPIO17 ESP-RX ← modem TX**, 115200 8N1 |
| APN | `airtelgprs.com` by default |

## Build and test

```bash
source ~/esp/esp-idf/export.sh   # ESP-IDF v5.2.3
cd firmware_v2/master
idf.py set-target esp32c6
idf.py build
```

Host tests:

```bash
cmake -S tests/host -B tests/host/build
cmake --build tests/host/build
ctest --test-dir tests/host/build --output-on-failure
```

## Layout

| Path | Role |
|---|---|
| `firmware_v2/master/` | Carrier master firmware (only ESP-IDF app) |
| `firmware_v2/host/` | How to build Zigbee sensor hosts |
| `hardware/fleet_telematics_carrier/` | PCB docs + host PlatformIO projects |
| `tools/carrier_console/` | USB provisioning web UI |
| `tools/telemetry_mock/` | Lab Trafyn-shaped uplink mock |
| `tools/ci/` | Bitbucket build/package scripts |
| `tests/host/` | Host-side unit tests against v2 sources |
| `docs/` | Specs, plans, archive |

## Active firmware modules

See [`firmware_v2/master/docs/MASTER.md`](firmware_v2/master/docs/MASTER.md).

## Non-goals in this tree

- Root `CMakeLists.txt` / `components/` / `main/` (deleted — use `firmware_v2/master`)
- SoftAP `.bin` upload as the primary OTA path
- BLE ELM327 adapter stack
