# UL212 RS-232 Fetch (ESP32-C6) — Design

**Date:** 2026-09-04  
**Status:** Approved for Phase 1  
**Board:** Seeed Studio XIAO ESP32-C6  
**Sibling projects:** `ul212-ble-fetch` (BLE + Zigbee), `ul212-wired-fetch` (lab notes / CH340 scripts)

## Goal

Phase 1: fetch UL212 fuel-level data over **RS-232 via MAX232** on the XIAO ESP32-C6 and print decoded readings on USB serial. Clean, simple code; Zigbee deferred to Phase 2.

## Non-goals (Phase 1)

- Zigbee / FleetZigbee / custom Zigbee partitions
- BLE
- NVS provisioning CLI
- Carrier console integration

## Hardware

```
XIAO C6 UART1 TX  →  MAX232 T1IN
XIAO C6 UART1 RX  ←  MAX232 R1OUT
MAX232 T1OUT      →  Sensor RX
Sensor TX         →  MAX232 R1IN
GND common; MAX232 VCC per module (5 V MAX232 / 3.3 V MAX3232)
USB CDC @ 115200 for logs
```

Exact GPIO numbers live in `board_pins.h` and README (XIAO C6 D6/D7 or equivalent UART1-capable pins).

## Software layout

```
hardware/fleet_telematics_carrier/host/ul212-rs232-fetch/
  platformio.ini
  include/
    board_pins.h
    ul212_rs232_config.h
    ul212_rs232.h
  src/
    main.cpp
    ul212_rs232.cpp
  README.md
```

### Naming

| Name | Meaning |
|------|---------|
| `ul212-rs232-fetch` | Wired RS-232 host app |
| `Ul212Reading` | Decoded sensor sample |
| `ul212Rs232Begin` / `ul212Rs232Poll` | Driver API |
| `UL212_RS232_PROTOCOL` | Compile-time 14 or 51 |

## Protocol switch

`include/ul212_rs232_config.h`:

```c
#ifndef UL212_RS232_PROTOCOL
#define UL212_RS232_PROTOCOL 14   /* default: full *XD frame */
#endif

#ifndef UL212_RS232_ADDRESS
#define UL212_RS232_ADDRESS 1
#endif
```

| Protocol | TX command (addr 1) | RX | Fields |
|----------|---------------------|-----|--------|
| **14** (default) | `$!RY0114\r\n` | `*XD,…#` | height, smooth, temp, tilt, signal |
| **51** | `$!RY0151\r\n` | `*CFV…` | height only |

TankOffline must match: address **1**, protocol **14** or **51**.

## Data model

```c
struct Ul212Reading {
  bool valid;
  float height_mm;    /* real-time (14) or CFV (51) */
  float smooth_mm;    /* 14 only; else 0 */
  float temp_c;       /* 14 only; else 0 */
  uint8_t tilt_deg;   /* 14 only; else 0 */
  uint8_t signal;     /* 14 only; else 0 */
  uint32_t stamp_ms;
};
```

Parse rules follow lab NOTES (`NOTES.md` in `ul212-wired-fetch`): 0.1 mm units; temp `(raw - 400) * 0.1`; tilt from last hex byte of protocol-14 trailer field.

## Driver behaviour

1. `ul212Rs232Begin(rx_pin, tx_pin, baud=9600)` — open `HardwareSerial` UART1.
2. `ul212Rs232Poll()`:
   - Build `$!RY` + addr + protocol + `\r\n`.
   - For **14:** send wake, accumulate until `#` or timeout; do not discard mid-frame; optional listen window if sensor auto-pushes.
   - For **51:** send, wait for `*CFV` + CRLF, timeout ~400–800 ms.
3. Return `Ul212Reading` with `valid` set on successful parse.

## `main.cpp`

- USB Serial 115200.
- Begin RS-232 on pins from `board_pins.h`.
- Loop: `poll` → print one line (height / smooth / temp / tilt / signal) → delay appropriate to protocol (e.g. ~1 s for 51, ~1–2 s listen cadence for 14 without flushing good data).

## Phase 2 (out of scope now)

Reuse `Ul212Reading` → Fleet Zigbee ED reports with reading IDs **16–21** (same mapping as `ul212-ble-fetch`). Enable Zigbee build flags / partitions only then.

## Success criteria (Phase 1)

1. Flash XIAO C6; USB monitor shows decoded lines when MAX232 + UL212 wired correctly.
2. Switching `UL212_RS232_PROTOCOL` to `51` (and app to 51) yields ~1 Hz height.
3. Code stays readable: clear names, short comments on wire protocol and pin map.
4. No Zigbee dependency in the default `platformio.ini`.

## References

- Lab proof: `host/ul212-wired-fetch/NOTES.md`
- BLE sibling: `host/ul212-ble-fetch/`
