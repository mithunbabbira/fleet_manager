# UL212 RS-232 Fetch — Phase 2 Zigbee Design

**Date:** 2026-09-04  
**Status:** Approved  
**Board:** Seeed Studio XIAO ESP32-C6  
**Builds on:** `2026-09-04-ul212-rs232-fetch-design.md` (Phase 1 RS-232)  
**Sibling:** `ul212-ble-fetch` (BLE + Zigbee)

## Goal

Add Fleet Zigbee end-device uplink to `ul212-rs232-fetch`: after a valid RS-232 reading, join the carrier on a configured channel and report UL212 metrics (reading IDs 16–21). Keep the code small, one config header for identity/RF, no NVS/CLI.

## Non-goals

- **Bluetooth / BLE entirely** — this host is wired RS-232 only. Do not link BLE stacks, scan, GATT, Modbus-over-BLE, or any BLE coexistence / airtime helpers from `ul212-ble-fetch`. Zigbee is the only radio.
- Extended PAN / install-code parent binding (deferred; reliability not proven on this stack)
- Carrier Console changes; protocol-51 as default
- Runtime provisioning of Zigbee IDs (NVS/CLI)

## Multi-truck segregation (channel-per-truck)

| Concern | Mechanism |
|---------|-----------|
| Join the correct truck’s carrier | **Same Zigbee channel** on that truck’s carrier + this host. Nearby trucks use **different** channels. |
| Segregate hosts in registry/cloud | Unique **`device_id`** (and `node_id`) per board |

Channel alone is RF isolation. `device_id` is application identity after join. Same channel + two open coordinators nearby can still cross-join; **do not share channel across trucks**.

## Config (only file to edit for Zigbee)

`include/zigbee_app_config.h`:

```c
#define FLEET_ZB_CHANNEL  15              /* must match that truck’s carrier */
#define ZB_DEVICE_ID      "ul212-rs232-001"
#define ZB_NODE_ID        "node-ul212-rs232-001"
#define ZB_SCHEMA_ID      "1088"
#define ZB_HOST_TYPE      "ul212_rs232_fetch"
```

Optional fixed metric map string (same as BLE defaults), built into the report path — not a runtime variable list.

Rebuild after edits. Truck 2 example: channel `20`, `device_id` `ul212-rs232-002`.

## Software layout

```
hardware/fleet_telematics_carrier/host/ul212-rs232-fetch/
  include/
    zigbee_app_config.h     /* NEW — channel + ids */
    zigbee_report.h         /* NEW */
    board_pins.h            /* unchanged */
    ul212_rs232*.h          /* unchanged */
  src/
    main.cpp                /* poll RS-232; stash latest; start zb task */
    ul212_rs232.cpp         /* unchanged */
    zigbee_report.cpp       /* NEW — begin + report loop */
  platformio.ini            /* Zigbee ED partitions + FleetZigbee libs */
  partitions_zigbee.csv     /* copy from ul212-ble-fetch */
  README.md                 /* document config + channel-per-truck */
```

Reuse shared libs: `FleetProtocol`, `FleetZigbee`, Arduino `Zigbee` (same as BLE host).

## Data model / report mapping

Reuse Phase 1 `Ul212Reading`. Map to Fleet readings (same as BLE):

| ID | Key | Type | Source |
|----|-----|------|--------|
| 16 | height_mm | float | `height_mm` |
| 17 | smooth_mm | float | `smooth_mm` |
| 18 | temperature_c | float | `temp_c` |
| 19 | signal | u8 | `signal` |
| 20 | valid_echo | u8 | `1` if `signal > 0`, else `0` |
| 21 | tilt_deg | u8 | `tilt_deg` |

Status bits: sensor connected (UART path always “connected” when polling) + reading valid when `r.valid`.

## Behaviour

1. **Main loop:** `ul212Rs232Poll` → update shared latest reading → USB print (Phase 1 cadence).
2. **Zigbee task:** wait for first `valid` reading → fill `FleetZigbeeConfig` from `zigbee_app_config.h` macros (no Preferences) → `fleetZigbeeEdBegin` → loop:
   - `fleetZigbeeEdLoop()`
   - if joined and latest valid and ~5 s elapsed → `fleetZigbeeEdSendReport(...)`
3. No Bluetooth anywhere in the firmware image or build (no BLE libs, no RF sharing with BLE).

Keep shared state minimal: one latest `Ul212Reading` + a validity flag (or stamp), protected only if needed for FreeRTOS (simple critical section or atomic copy).

## Build

`platformio.ini` gains (mirror BLE host):

- `board_build.partitions = partitions_zigbee.csv`
- `lib_extra_dirs = ../lib`
- `lib_deps = FleetProtocol, FleetZigbee, Zigbee`
- flags: `ZIGBEE_MODE_ED`, `FLEET_ZIGBEE_ED_RADIO=1`, plus existing USB CDC flags
- Ensure `FLEET_ZB_CHANNEL` from `zigbee_app_config.h` wins over library default (include/order or `-include`)

## Success criteria

1. Flash host with defaults; carrier on channel 15; USB shows RS-232 lines; `[zb] joined` / reports; `fleet hosts` shows `ul212-rs232-001` with height/etc.
2. Changing only `FLEET_ZB_CHANNEL` (host) and carrier channel to a second value joins that network; host does not join a truck left on channel 15.
3. Code stays readable: one Zigbee config header, one short report module, no CLI/NVS.

## Follow-ups (explicitly later)

- Extended PAN / install-code parent binding if same-channel multi-truck is required
- Optional USB CLI later if field re-ID without rebuild is needed

## References

- Phase 1: `docs/superpowers/specs/2026-09-04-ul212-rs232-fetch-design.md`
- Host guide: `docs/fleet-zigbee-host-guide.md`
- BLE report mapping: `host/ul212-ble-fetch/src/zigbee_report.cpp`
- Carrier channel: `CONFIG_FLEET_ZIGBEE_CHANNEL`
