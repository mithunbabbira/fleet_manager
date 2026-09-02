# ESP32-C6 Fleet Telematics — Project Context

> Last updated: 2026-09-02 (Carrier Console; on-device HTTP off by default).
>
> Production data path: vehicle CAN through MCP2515; optional Zigbee sensor hosts
> via `transport_zigbee` → `host_registry`. Local access: USB Serial/JTAG plus
> PC **Carrier Console** (`tools/carrier_console`, port 8766). Cloud: Quectel EC200U
> over **QHTTP AT** (no PPP). The BLE ELM327 adapter stack has been deleted.
>
> Authoritative pin map:
> `hardware/fleet_telematics_carrier/README.md`
> Fabricated gerbers: `Vehical_Telematics_Design.zip` (KiCad 9, 2026-08-12).
>
> Flash layout is dual-bank OTA (`ota_0` + `ota_1`). LTE firmware check: Trafyn
> POST `get-latest-device-firmware` (see
> `docs/superpowers/specs/2026-08-19-trafyn-firmware-ota-design.md`).

## Product

ESP-IDF firmware (CMake project name `elm327_esp32c6`) for an ESP32-C6 Super Mini. It:

- detects ISO 15765-4 CAN at 11/29-bit identifiers and 500/250 kbit/s;
- polls profile-driven OBD PIDs and decodes PIDs, DTCs, and VIN;
- enforces `cmd_policy` before sending vehicle commands;
- exposes status, control, and telemetry through USB serial and the PC Carrier Console;
- posts telemetry through an EC200U using Quectel QHTTP AT commands (HTTPS);
- optionally ingests Zigbee host TLV reports as separate schema-1088 uplink events;

The default profile is `fleet_basic`. The CAN path was validated in-car on
2026-07-29 (CAN11/500, live RPM/speed/coolant/throttle). The printed carrier
was validated on the bench on 2026-08-18 (MCP SPI, microSD, LTE AT on Airtel).
Zigbee coordinator + UL212 host ED were validated on the bench 2026-09-01
(open network, ~1 Hz TLV ingest).

## Hardware (printed PCB — what firmware uses)

| Item | Detail |
|---|---|
| MCU | ESP32-C6 Super Mini, 4 MB flash |
| CAN | MCP2515 over **soft-SPI** through TXS0108E; GPIO21 SCK, 22 MOSI, 23 MISO, 20 CS, 14 INT |
| TXS map | A1/B1 CS, A2/B2 SO/MISO, A3/B3 SI/MOSI, A4/B4 SCK, A5/B5 INT |
| microSD | Hardware SPI2 GPIO4 SCK, 5 MOSI, 6 MISO, 18 CS |
| Console | USB Serial/JTAG, 115200 8N1; PC UI at `tools/carrier_console` |
| Wi-Fi | Disabled by default (`CONFIG_ELM_HTTP_ENABLE=n`); optional legacy SoftAP |
| LTE | EC200U UART1: **GPIO16 ESP-TX → modem RX**, **GPIO17 ESP-RX ← modem TX**, 115200 8N1 |
| APN | `airtelgprs.com` by default |

The modem needs its own VBAT, PWRKEY, and common ground. UART1 is reserved for
LTE. Firmware UART pins match the printed copper; **do not respin the PCB**
for RX/TX, and **do not flash** images that still use TX=17 / RX=16.

Not used: jumper-wire prototype harness, shared MCP+SD SPI bus, textbook
GPIO17=ESP-TX / GPIO16=ESP-RX map.

## Build and test

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

Host tests:

```bash
cmake -S tests/host -B tests/host/build
cmake --build tests/host/build
ctest --test-dir tests/host/build --output-on-failure
```

Important defaults:

- ESP32-C6 target, 4 MB flash, custom `partitions.csv`;
- USB Serial/JTAG console;
- Bluetooth disabled; on-device HTTP/SoftAP disabled by default;
- OBD command timeout 12 seconds;
- LTE enabled with the Airtel APN and UART TX=16 / RX=17.

## Active components

```text
components/
├── can_obd/             MCP2515, ISO-TP, protocol detection, OBD transaction API
├── cmd_policy/          read-only allowlist gate
├── fleet_protocol/      shared fleet_tlv codec
├── host_registry/       Zigbee host manifest catalog + runtime registry
├── net_lte/             EC200U UART control and QHTTP transport
├── obd_codec/           PID, DTC, and VIN decoding
├── obd_poller/          profile-driven poll task and raw-command queue
├── profile_store/       NVS profiles and safety settings
├── store_sd/            microSD (SPI CS18) durable uplink queue
├── sys_runtime/         watchdog, metrics, and OTA stub
├── telemetry_bus/       in-process typed pub/sub
├── telemetry_uplink/    LTE cloud payload, SD enqueue, batch drain
├── transport_http/      optional SoftAP + REST (Kconfig ELM_HTTP_ENABLE)
├── transport_serial/    USB interactive console (primary config)
└── transport_zigbee/    optional Zigbee coordinator (ESP-Zigbee-SDK)
```

Off-tree host firmware lives under `hardware/fleet_telematics_carrier/host/`
(see `docs/fleet-zigbee-host-guide.md`).

## Data flow

```text
MCP2515 → can_obd → obd_poller → telemetry_bus
                           ├──→ transport_serial (+ PC Carrier Console)
                           ├──→ transport_http (optional)
                           ├──→ transport_zigbee → host_registry
                           └──→ telemetry_uplink → store_sd → net_lte → cloud
                                              (or live POST if SD missing)

profile_store → obd_poller / command safety / uplink configuration
sys_runtime   → watchdog and metrics across the application
```

`can_obd` owns vehicle transactions. Serial and HTTP submit raw requests through
`obd_poller`, which applies command policy. The poller starts after CAN protocol
detection and pauses while the CAN link is unavailable.

## Boot flow

1. Initialize NVS, runtime metrics/watchdog, profiles, and telemetry bus.
2. Start LTE (OTA auto-check); failures are non-fatal.
3. Init SPI CS idle-high → MCP2515 soft-SPI (`can_obd`) → microSD SPI2 (`store_sd`).
4. Start telemetry uplink (produce→SD queue, drain→batch POST).
5. Start serial transport; confirm OTA if pending. Optional SoftAP/HTTP if enabled.
6. Start `obd_poller` paused; CAN boot task enables it when an ECU responds.

## Interfaces

Serial commands: `help`, `config`, `status`, `cmd`, `vin`, `dtc`, `profiles`,
`profile`, `telemetry`, `unsafe`, `metrics`, `lte`, `uplink`, `ota`, `fleet hosts`,
and `fleet ingest`.

Optional legacy HTTP (when `CONFIG_ELM_HTTP_ENABLE=y`) serves the embedded UI plus
status, profile, telemetry, LTE, uplink, VIN, DTC, and OBD APIs — see
`components/transport_http/http_api.c`.

## LTE uplink

`telemetry_uplink` emits **multiple typed events** per tick (OBD `1087`, GPS `1089`,
host reading `1088`) with envelope `{device_id, node_id, schemaId, ts_ms, payload}`.
`ts_ms` uses modem wall-clock when available (`AT+CCLK?` network time, else GPS UTC
from the existing `QGPSLOC` poll); falls back to uptime ms until first sync.
Events queue one-per-line on microSD and batch-POST as a JSON array. See
`docs/telemetry-api-backend-guide.md`.

Bench 2026-08-18: modem registered on Airtel (`csq=18`); HTTP left the module
(server 400 is an API/payload issue, not a UART failure). Backend must register
schemas `1088` and `1089` for host and GPS events.

## Fleet Zigbee

Optional second ESP32-C6 hosts join an **open** Zigbee network on channel 15 and
send `fleet_tlv` frames on custom cluster `0xFC00`. Coordinator ingest updates
`host_registry`; each valid host reading becomes a separate schema-`1088` uplink
event (not a nested `hosts[]` array).

Docs: `docs/fleet-zigbee-host-guide.md`, `docs/fleet-zigbee-coexistence.md`.

## Known follow-up work

- Confirm OBD ECU replies on a live vehicle CAN bus with this PCB.
- Cloud: register and ingest schemas `1088` (host readings) and `1089` (GPS).
- Zigbee rejoin after power-cycle and multi-host soak tests.
- Keep legacy NVS namespace/key names only where changing them would require an
  explicit migration.
