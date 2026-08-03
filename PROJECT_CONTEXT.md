# ESP32-C6 Fleet Telematics — Project Context

> Last updated: 2026-08-03 on `feature/mcp2515-can`.
>
> The production data path is direct vehicle CAN through MCP2515. Local access is
> USB Serial/JTAG plus Wi-Fi SoftAP/HTTP, and cloud telemetry uses a Quectel EC200U
> LTE modem. The retired wireless OBD adapter stack has been deleted.
>
> Flash layout is dual-bank OTA (`ota_0` + `ota_1`). Lab manifest + `.bin` host:
> `tools/ota_dev_server/`. Design: `docs/superpowers/specs/2026-07-31-lte-ota-design.md`.

## Product

ESP-IDF firmware (CMake project name `elm327_esp32c6`) for an ESP32-C6 Mini. It:

- detects ISO 15765-4 CAN at 11/29-bit identifiers and 500/250 kbit/s;
- polls profile-driven OBD PIDs and decodes PIDs, DTCs, and VIN;
- enforces `cmd_policy` before sending vehicle commands;
- exposes status, control, and telemetry through USB and a SoftAP web UI/API;
- posts telemetry through an EC200U using Quectel HTTPS commands.

The default profile is `fleet_basic`. The CAN path was validated in-car on
2026-07-29 with CAN11/500 and live RPM, speed, coolant, and throttle data.

## Hardware

| Item | Detail |
|---|---|
| MCU | ESP32-C6 Mini, 4 MB flash |
| CAN | MCP2515 over SPI; GPIO20 MOSI, GPIO21 MISO, GPIO22 SCLK, GPIO23 CS, GPIO14 INT |
| Console | USB Serial/JTAG, 115200 8N1 |
| Wi-Fi | SoftAP `Fleet-C6`, password `fleetc6`, UI at `http://192.168.4.1/` |
| LTE | Quectel EC200U on UART1; GPIO17 TX, GPIO16 RX, 115200 8N1 |
| APN | `airtelgprs.com` by default |

The modem requires its own suitable supply, PWRKEY sequencing, and common ground.
UART1 is reserved for LTE.

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
- LTE enabled with the Airtel APN above.

## Active components

```text
components/
├── can_obd/             MCP2515, ISO-TP, protocol detection, OBD transaction API
├── cmd_policy/          read-only allowlist gate
├── net_lte/             EC200U UART control and HTTPS transport
├── obd_codec/           PID, DTC, and VIN decoding
├── obd_poller/          profile-driven poll task and raw-command queue
├── profile_store/       NVS profiles and safety settings
├── sys_runtime/         watchdog, metrics, and OTA stub
├── telemetry_bus/       in-process typed pub/sub
├── telemetry_uplink/    LTE cloud payload and send scheduling
├── transport_http/      SoftAP, REST API, and embedded web UI
└── transport_serial/    USB interactive console
```

The old wireless adapter transport, client, and radio components no longer
exist or participate in CMake.

## Data flow

```text
MCP2515 → can_obd → obd_poller → telemetry_bus
                           ├──→ transport_serial
                           ├──→ transport_http
                           └──→ telemetry_uplink → net_lte → cloud

profile_store → obd_poller / command safety / uplink configuration
sys_runtime   → watchdog and metrics across the application
```

`can_obd` owns vehicle transactions. Serial and HTTP submit raw requests through
`obd_poller`, which applies command policy. The poller starts after CAN protocol
detection and pauses while the CAN link is unavailable.

## Boot flow

1. Initialize NVS, runtime metrics/watchdog, profiles, and telemetry bus.
2. Start LTE and telemetry uplink; failures are non-fatal.
3. Start serial and SoftAP/HTTP transports.
4. Initialize and start `can_obd` protocol detection.
5. Start `obd_poller` paused.
6. The CAN boot task enables polling when an ECU responds and pauses it on loss.

## Interfaces

Serial commands: `help`, `status`, `cmd`, `profiles`, `profile`, `telemetry`,
`unsafe`, `metrics`, `lte`, and `uplink`.

HTTP serves the embedded UI plus status, protocol, profile, telemetry, metrics,
LTE, uplink, VIN, DTC, safety, health, and raw OBD command APIs. See
`components/transport_http/http_api.c` for the authoritative route list.

## LTE uplink

`telemetry_uplink` posts OBD snapshots to the configured fleet endpoint through
the EC200U. It is disabled by default and can be controlled from the SoftAP UI,
REST API, or serial console. Sends are gated on CAN readiness, polling, and fresh
samples; missing values are represented explicitly in the payload.

## Known follow-up work

- Validate the full build, image size, and hardware behavior after stack removal.
- Complete production OTA support (`sys_runtime` currently exposes a stub).
- Keep legacy NVS namespace/key names only where changing them would require an
  explicit migration.
