# UL212 Dummy Zigbee Host — Design

**Date:** 2026-09-05  
**Status:** Implemented (build OK; HW join verify on device)  
**Board:** ESP32-C6 Super Mini (PlatformIO: `esp32-c6-devkitc-1` + USB CDC)  
**Depends on:** Carrier Zigbee coordinator (ch 15 + EPAN); FleetZigbee ED stack  
**Sibling:** `ul212-rs232-fetch`, `ul212-ble-fetch`

## Goal

Lab/car smoke host: an ESP32-C6 with **no fuel sensor** joins the carrier Zigbee network and periodically sends **synthetic UL212-shaped readings** so master `fleet hosts` and uplink schema **1088** can be tested without RS-232/BLE hardware.

## Non-goals

- Real UL212 / Modbus / BLE
- SoftAP / NVS provisioning CLI
- Changing carrier firmware or EPAN scheme
- Install codes / timed permit-join
- Production device IDs (lab IDs only)

## Approach

**New PlatformIO project** `hardware/fleet_telematics_carrier/host/ul212-dummy-zigbee/` that:

1. Starts FleetZigbee ED with the same channel + Extended PAN ID as the lab carrier.
2. Generates slowly varying fake height/smooth/temp/signal/tilt (no UART wait).
3. Sends HELLO once joined, then REPORT every ~5 s with the same TLV metric IDs as RS-232 host.

Reuses shared libs under `host/lib/` (`FleetZigbee`, `FleetProtocol`). Does **not** fork Zigbee protocol.

## Config (edit + rebuild)

| Knob | Lab default | Notes |
|------|-------------|--------|
| Channel | `15` | Must match carrier `CONFIG_FLEET_ZIGBEE_CHANNEL` |
| EPAN | `F1EE700000000001` | Must match carrier `CONFIG_FLEET_ZIGBEE_EPAN_ID` |
| `device_id` | `ul212-dummy-001` | Unique vs real hosts |
| `node_id` | `node-ul212-dummy-001` | |
| `schemaId` | `1088` | |
| `host_type` | `ul212_dummy_zigbee` | Visible in registry |
| Metric map | Same TLV ids 16–21 as RS-232 host | height/smooth/temp/signal/valid_echo/tilt |

## Synthetic readings

- Base height ~80 mm, slow sine ±10 mm over ~60 s (so `fleet hosts` / cloud show change).
- `smooth_mm` ≈ height; `temperature_c` ≈ 30 ± 2; `signal` = 30; `valid_echo` = 1; `tilt_deg` = 3–5.
- Always mark reading valid so Zigbee task never blocks waiting for a sensor.

## Software layout

```
hardware/fleet_telematics_carrier/host/ul212-dummy-zigbee/
  platformio.ini          # env: esp32-c6-devkitc-1 (Super Mini)
  partitions_zigbee.csv   # copy/symlink same as rs232 host
  include/zigbee_app_config.h
  src/main.cpp            # generate fake Ul212Reading + zigbee report
  src/zigbee_report.cpp   # thin copy or shared pattern from rs232 (no RS-232 wait)
  include/zigbee_report.h
  README.md               # flash + join checklist
```

Prefer **local copies** of `zigbee_report.*` adapted to drop “wait for first RS-232 reading,” rather than coupling to the RS-232 project.

## Board / flash

- Default env: `esp32-c6-devkitc-1` with `ARDUINO_USB_CDC_ON_BOOT`, Zigbee ED partitions, `lib_extra_dirs = ../lib`, `lib_ldf_mode = chain+`.
- Optional second env: `seeed_xiao_esp32c6` if someone flashes a XIAO later.
- If Super Mini upload fails, document hold-BOOT / different USB port; no pin map required (USB-only app).

## Success criteria

1. `pio run -e esp32-c6-devkitc-1` builds.
2. Flash Super Mini; USB log shows Zigbee start + join (same EPAN/ch as carrier).
3. Carrier `fleet hosts` shows `ul212-dummy-001` with `link=1` and changing `height_mm`.
4. With LTE uplink, mock/Trafyn can receive schema **1088** for that device (when produce tick runs).

## Out of scope follow-ups

- CLI to set height at runtime  
- Battery/deep-sleep  
- Multiple dummy identities without rebuild  
