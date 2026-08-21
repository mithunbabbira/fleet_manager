# MCP2515 Direct-CAN OBD Transport (replaces ELM327 on this branch)

Branch: `feature/mcp2515-can` · Date: 2026-07-29

## Goal

Fetch OBD data straight from the vehicle CAN bus with an MCP2515 (SPI) +
TJA1050 module and an OBD-II cable — no ELM327, no BLE — and keep the
existing decode → telemetry → LTE uplink pipeline working unchanged.

Deployment targets: currently a passenger car (validated: ISO 15765-4,
11-bit, 500 kbit/s, ECU 0x7E8), then trucks — first an Ashok Leyland
3718G BSIV (2018), which may use 250 kbit and/or 29-bit addressing.
**Therefore the protocol must NOT be hardcoded.**

## Hardware (validated in-car 2026-07-29)

ESP32-C6 Super Mini ↔ TXS0108E (3.3 V/5 V) ↔ MCP2515+TJA1050 (8 MHz xtal)

| Signal | ESP GPIO |
|--------|----------|
| SCK    | 21 |
| MOSI   | 22 |
| MISO   | 23 |
| CS     | 20 |
| INT    | 14 |

EC200U LTE is UART1 on the printed PCB: **GPIO16 ESP-TX → modem RX**, **GPIO17 ESP-RX ← modem TX** (firmware defaults match this copper). OBD: CAN_H pin 6, CAN_L pin 14, GND pin 4/5. See `hardware/fleet_telematics_carrier/README.md`.

## Architecture

```
vehicle CAN ── MCP2515 ──SPI── can_obd ──text API──► obd_poller ──► telemetry_bus
                                  │                                     │
                     protocol autodetect + NVS                 http UI / serial /
                                                              telemetry_uplink → LTE
```

### New component `components/can_obd/`

- `mcp2515.c/.h` — register-level driver: reset, bit timing (8/16 MHz xtal via
  Kconfig), RX filters, send/receive, mode control. Polled RX (INT pin reserved
  for later).
- `obd_isotp.c/.h` — **pure C, host-testable**: build single-frame requests from
  hex command strings ("010C"), reassemble single/multi-frame responses
  (first frame → caller sends flow control), emit payload as uppercase hex
  ("410C0C30") — the same shape `elm327_client_transact` returned, so
  `obd_codec`, profiles, and uplink raw fields keep working.
- `can_obd.c` — glue: init/start, protocol autodetect, link supervision task,
  `can_obd_transact(cmd, resp, len, timeout_ms)` mutex single-flight API.

### Protocol handling (NOT hardcoded)

Candidate list, tried in order until an ECU answers `0100`:

1. 11-bit / 500 kbit (`0x7DF` → `0x7E8..0x7EF`)  — modern cars
2. 11-bit / 250 kbit
3. 29-bit / 500 kbit (`0x18DB33F1` → `0x18DAF1xx`)
4. 29-bit / 250 kbit — common on heavy vehicles

- Kconfig `CAN_OBD_PROTOCOL`: `auto` (default) or pinned to one candidate.
- Last working protocol persisted in NVS (`elm` namespace, key `can_proto`);
  tried first on next boot, falls back to full sweep after repeated failures.
- Link supervision: probe `0100` at start; on repeated transact failures the
  link is marked down and the sweep restarts with backoff (handles ignition
  off/on and moving the device between vehicles).
- Reported protocol string, e.g. `ISO15765-4 CAN11/500` / `ISO15765-4 CAN29/250`,
  exposed via `can_obd_get_protocol()` for UI + uplink payload.
- J1939 (truck broadcast PGNs) is explicitly out of scope for v1; noted as a
  follow-up if the Ashok Leyland's OBD gateway doesn't serve ISO 15765 PIDs.

### Integration changes

- `obd_poller`: calls `can_obd_transact` / `can_obd_is_ready` instead of the
  ELM client; profile items starting with `AT` are skipped (logged once) —
  `ATRV` battery voltage has no CAN equivalent, so `voltage_v` uplinks as null.
- `main/app_main.c`: BLE bonded-boot replaced by a `can_boot` task that waits
  for `can_obd` link-up then enables the poller. BLE/NimBLE stays compiled and
  initialized (transports reference it) but no connection is attempted.
- `telemetry_uplink`: gate becomes `can_obd_is_ready() && poller_on && fresh
  sample`; payload keeps schema keys but `adapter_name="MCP2515"`,
  `ble_peer_address="-"`, `ble_connected`/`elm_ready` mirror CAN link state,
  `obd_protocol` from `can_obd_get_protocol()`.
- `cmd_policy` unchanged — still gates every command in the poller.

### Testing

- Host: `tests/host/test_obd_isotp.c` — SF build, SF/FF+CF reassembly, hex
  output, padding, error paths.
- Device: in-car — poller streams rpm/speed/coolant/throttle, web UI shows
  live values, uplink POSTs 200 to api.trafyn.info over LTE.
- Truck validation: plug into Ashok Leyland, autodetect should land on a
  250 kbit and/or 29-bit candidate without code changes.
