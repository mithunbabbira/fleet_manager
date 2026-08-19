# ESP32-C6 Fleet Telematics — Project Context

> Last updated: 2026-08-18 on the printed carrier PCB.
>
> Production data path: vehicle CAN through MCP2515. Local access: USB
> Serial/JTAG plus Wi-Fi SoftAP/HTTP. Cloud: Quectel EC200U. The BLE ELM327
> adapter stack has been deleted.
>
> Authoritative pin map:
> `hardware/fleet_telematics_carrier/README.md`
> Fabricated gerbers: `Vehical_Telematics_Design.zip` (KiCad 9, 2026-08-12).
>
> Flash layout is dual-bank OTA (`ota_0` + `ota_1`). LTE firmware check: Trafyn
> POST `get-latest-device-firmware` (see
> `docs/superpowers/specs/2026-08-19-trafyn-firmware-ota-design.md`). SoftAP
> does not upload firmware.

## Product

ESP-IDF firmware (CMake project name `elm327_esp32c6`) for an ESP32-C6 Super Mini. It:

- detects ISO 15765-4 CAN at 11/29-bit identifiers and 500/250 kbit/s;
- polls profile-driven OBD PIDs and decodes PIDs, DTCs, and VIN;
- enforces `cmd_policy` before sending vehicle commands;
- exposes status, control, and telemetry through USB and a SoftAP web UI/API;
- posts telemetry through an EC200U using Quectel HTTPS commands.

The default profile is `fleet_basic`. The CAN path was validated in-car on
2026-07-29 (CAN11/500, live RPM/speed/coolant/throttle). The printed carrier
was validated on the bench on 2026-08-18 (MCP SPI, microSD, LTE AT on Airtel).

## Hardware (printed PCB — what firmware uses)

| Item | Detail |
|---|---|
| MCU | ESP32-C6 Super Mini, 4 MB flash |
| CAN | MCP2515 over **soft-SPI** through TXS0108E; GPIO21 SCK, 22 MOSI, 23 MISO, 20 CS, 14 INT |
| TXS map | A1/B1 CS, A2/B2 SO/MISO, A3/B3 SI/MOSI, A4/B4 SCK, A5/B5 INT |
| microSD | Hardware SPI2 GPIO4 SCK, 5 MOSI, 6 MISO, 18 CS |
| Console | USB Serial/JTAG, 115200 8N1 |
| Wi-Fi | SoftAP `Fleet-C6`, password `fleetc61`, UI at `http://192.168.4.1/` |
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
- Bluetooth disabled; SoftAP enabled;
- OBD command timeout 12 seconds;
- LTE enabled with the Airtel APN and UART TX=16 / RX=17.

## Active components

```text
components/
├── can_obd/             MCP2515, ISO-TP, protocol detection, OBD transaction API
├── cmd_policy/          read-only allowlist gate
├── net_lte/             EC200U UART control and HTTPS transport
├── obd_codec/           PID, DTC, and VIN decoding
├── obd_poller/          profile-driven poll task and raw-command queue
├── profile_store/       NVS profiles and safety settings
├── store_sd/            microSD (SPI CS18) durable uplink queue
├── sys_runtime/         watchdog, metrics, and OTA stub
├── telemetry_bus/       in-process typed pub/sub
├── telemetry_uplink/    LTE cloud payload, SD enqueue, batch drain
├── transport_http/      SoftAP, REST API, and embedded web UI
└── transport_serial/    USB interactive console
```

## Data flow

```text
MCP2515 → can_obd → obd_poller → telemetry_bus
                           ├──→ transport_serial
                           ├──→ transport_http
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
5. Start serial and SoftAP/HTTP transports; confirm OTA if pending.
6. Start `obd_poller` paused; CAN boot task enables it when an ECU responds.

## Interfaces

Serial commands: `help`, `status`, `cmd`, `profiles`, `profile`, `telemetry`,
`unsafe`, `metrics`, `lte`, and `uplink`.

HTTP serves the embedded UI plus status, protocol, profile, telemetry, metrics,
LTE, uplink, VIN, DTC, safety, health, and raw OBD command APIs. See
`components/transport_http/http_api.c` for the authoritative route list.

## LTE uplink

`telemetry_uplink` builds OBD snapshots (including optional `lat`/`lng`/`gps_ok`
from the EC200U GNSS cache when fixed) and enqueues them on microSD
(`/sdcard/uplinkq.dat`). A drain task batch-POSTs events to the fleet
endpoint through the EC200U and removes records only after HTTP 2xx. If the SD
card is missing, it falls back to live single-event POST. SoftAP/serial status
exposes queue depth, SD mount state, and GNSS fix (`uplink` command). See
`docs/superpowers/specs/2026-08-06-sd-uplink-queue-design.md`.

Bench 2026-08-18: modem registered on Airtel (`csq=18`); HTTP left the module
(server 400 is an API/payload issue, not a UART failure).

## Known follow-up work

- Confirm OBD ECU replies on a live vehicle CAN bus with this PCB.
- Production OTA beyond the current LTE/SoftAP lab path.
- Keep legacy NVS namespace/key names only where changing them would require an
  explicit migration.
