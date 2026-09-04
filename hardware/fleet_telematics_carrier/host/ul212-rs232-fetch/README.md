# UL212 RS-232 Fetch

Host app for **Seeed XIAO ESP32-C6** + **MAX232** → **Tenet UL212** over wired RS-232.
Polls the sensor, decodes height (and optional temp/tilt/signal), prints lines on **USB serial @ 115200**,
and reports readings to the fleet carrier over **Zigbee** (reading IDs **16–21**).

**No Bluetooth** — this build is wired RS-232 + Zigbee only. No BLE stack, scan, GATT, or NVS/CLI provisioning.

## What it does

- UART1 @ 9600 8N1 through MAX232 to the sensor
- Compile-time protocol **14** (default, full `*XD` frame) or **51** (height-only `*CFV`, ~1 Hz)
- `ul212Rs232Begin` / `ul212Rs232Poll` driver; `main.cpp` prints decoded readings and stashes the latest for Zigbee
- Zigbee end-device task joins the carrier and sends Fleet TLV reports (~5 s when joined and reading valid)

Design specs:

- Phase 1 (RS-232): [`docs/superpowers/specs/2026-09-04-ul212-rs232-fetch-design.md`](../../../../docs/superpowers/specs/2026-09-04-ul212-rs232-fetch-design.md)
- Phase 2 (Zigbee): [`docs/superpowers/specs/2026-09-04-ul212-rs232-zigbee-design.md`](../../../../docs/superpowers/specs/2026-09-04-ul212-rs232-zigbee-design.md)

## Wiring

```
XIAO C6 D6 / GPIO16 (UART1 TX)  →  MAX232 T1IN
XIAO C6 D7 / GPIO17 (UART1 RX)  ←  MAX232 R1OUT
MAX232 T1OUT                    →  Sensor RX
Sensor TX                       →  MAX232 R1IN
GND common (XIAO + MAX232 + sensor)
MAX232 VCC: 5 V (MAX232) or 3.3 V (MAX3232 module)
```

Pin defines live in `include/board_pins.h` (`UL212_UART_TX_PIN=16`, `UL212_UART_RX_PIN=17`).

**Optional smoke test (no sensor):** short **MAX232 T1OUT ↔ R1IN** and confirm UART loopback at 9600 before wiring the UL212.

## Zigbee configuration

Edit **only** `include/zigbee_app_config.h` per board / truck, then rebuild. No other source file needs changes for identity or RF.

```c
#define FLEET_ZB_CHANNEL  15              /* must match that truck's carrier */
#define ZB_DEVICE_ID      "ul212-rs232-001"
#define ZB_NODE_ID        "node-ul212-rs232-001"
#define ZB_SCHEMA_ID      "1088"
#define ZB_HOST_TYPE      "ul212_rs232_fetch"
```

### Channel-per-truck

| Concern | Rule |
|---------|------|
| Join the correct truck's carrier | Set **`FLEET_ZB_CHANNEL`** to the **same channel** as that truck's carrier (`CONFIG_FLEET_ZIGBEE_CHANNEL`). |
| Nearby trucks | Use **different channels** on each truck so hosts cannot join the wrong parent. **Do not share a channel across trucks.** |
| Registry / cloud identity | Give each board a **unique `device_id`** (and matching `node_id`). |

Example — truck 2: channel `20`, `ZB_DEVICE_ID` `"ul212-rs232-002"`, `ZB_NODE_ID` `"node-ul212-rs232-002"`.

Carrier must have Zigbee enabled (`CONFIG_FLEET_ZIGBEE_ENABLE=y`) on the matching channel. After join, `fleet hosts` on the carrier USB serial should show your `device_id` with height and related readings.

## TankOffline settings

Configure the vendor **TankOffline** app to match the firmware:

| Setting | Value |
|---------|-------|
| Address | **1** |
| Protocol (full fields) | **14** |
| Protocol (height-only, pollable ~1 Hz) | **51** |

TankOffline protocol must match `UL212_RS232_PROTOCOL` in the build. Mismatch → no reply on the wire.

Lab proof, example frames, and decode rules: [`../ul212-wired-fetch/NOTES.md`](../ul212-wired-fetch/NOTES.md).

## Protocol switch

Default is protocol **14** in `include/ul212_rs232_config.h`:

```c
#define UL212_RS232_PROTOCOL 14   /* or 51 */
#define UL212_RS232_ADDRESS  1
```

To build for protocol **51** without editing the header, add to `platformio.ini` under `[env]` → `build_flags`:

```ini
  -DUL212_RS232_PROTOCOL=51
```

| Protocol | Poll (addr 1) | RX frame | Fields |
|----------|---------------|----------|--------|
| **14** | `$!RY0114\r\n` | `*XD,…#` | height, smooth, temp, tilt, signal |
| **51** | `$!RY0151\r\n` | `*CFV…` | height only (~1 Hz) |

## Build / upload / monitor

Use PlatformIO from this directory (`hardware/fleet_telematics_carrier/host/ul212-rs232-fetch/`):

```bash
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/cu.usbmodem*
~/.platformio/penv/bin/pio device monitor -b 115200 --port /dev/cu.usbmodem*
```

On boot you should see `protocol=… address=…` and `zigbee device=… ch=… (no Bluetooth)`, then lines like:

```text
height=36.3 mm  smooth=36.4 mm  temp=25.1 C  tilt=5 deg  signal=23
```

When Zigbee joins you should see `[zb] joined` (or similar) in the USB log; carrier `fleet hosts` lists the configured `device_id`.

Protocol **51** prints height with other fields zero; protocol **14** may also auto-push every ~8–10 s between polls.

## Manual checklist

1. **MAX232 loopback:** T1OUT ↔ R1IN shorted; bytes echo at 9600 (optional, no sensor).
2. **Sensor wired; TankOffline** address **1**, protocol **14** or **51** matches the build define.
3. **USB monitor** shows valid decoded lines (not repeated `(no frame)`).
4. **`zigbee_app_config.h`:** channel matches carrier; unique `device_id` for this board.
5. **Carrier:** same Zigbee channel; `fleet hosts` shows the host after join.

## Layout

```
ul212-rs232-fetch/
├── include/
│   ├── board_pins.h           /* UART pins: D6/GPIO16 TX, D7/GPIO17 RX */
│   ├── ul212_rs232_config.h   /* protocol + sensor address */
│   ├── ul212_rs232.h
│   ├── ul212_rs232_parse.h
│   ├── zigbee_app_config.h    /* ONLY file to edit for Zigbee identity/RF */
│   └── zigbee_report.h
├── src/
│   ├── main.cpp               /* poll RS-232; stash latest; start zb task */
│   ├── ul212_rs232.cpp
│   ├── ul212_rs232_parse.c
│   └── zigbee_report.cpp      /* Zigbee begin + report loop */
├── host_tests/test_parse.c
├── partitions_zigbee.csv      /* Zigbee ED partition table */
├── platformio.ini             /* FleetZigbee + Zigbee ED build flags */
└── README.md
```

Shared libraries: `../lib/FleetProtocol`, `../lib/FleetZigbee`, Arduino `Zigbee` (same stack as `ul212-ble-fetch`, but **no Bluetooth** in this firmware).

## Report mapping (IDs 16–21)

| ID | Key | Source |
|----|-----|--------|
| 16 | height_mm | `height_mm` |
| 17 | smooth_mm | `smooth_mm` |
| 18 | temperature_c | `temp_c` |
| 19 | signal | `signal` |
| 20 | valid_echo | `1` if `signal > 0`, else `0` |
| 21 | tilt_deg | `tilt_deg` |

Same mapping as `ul212-ble-fetch`; see Phase 2 spec for behaviour details.
