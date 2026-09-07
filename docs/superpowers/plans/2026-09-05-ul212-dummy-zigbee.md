# UL212 Dummy Zigbee Host Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** New PlatformIO Zigbee ED that joins the lab carrier and sends synthetic UL212-shaped 1088 reports for car/lab smoke tests.

**Architecture:** Clone the RS-232 host Zigbee report path without UART; `main` synthesizes slowly varying readings and feeds `zigbeeReportUpdate`; FleetZigbee ED uses same EPAN/channel/metric map as carrier lab config.

**Tech Stack:** PlatformIO Arduino ESP32-C6 (`esp32-c6-devkitc-1`), FleetZigbee, FleetProtocol, Zigbee ED partitions.

**Spec:** `docs/superpowers/specs/2026-09-05-ul212-dummy-zigbee-design.md`

## Global Constraints

- Channel `15`, EPAN `F1EE700000000001` (match carrier)
- `device_id` `ul212-dummy-001`, schema `1088`
- No RS-232/BLE; USB CDC logs only
- Reuse `../lib` FleetZigbee / FleetProtocol; `lib_ldf_mode = chain+`

## File map

| Path | Role |
|------|------|
| `hardware/.../ul212-dummy-zigbee/platformio.ini` | Super Mini env |
| `partitions_zigbee.csv` | Copy from rs232 host |
| `include/zigbee_app_config.h` | IDs / EPAN / metrics |
| `include/dummy_reading.h` | Local `Ul212Reading`-shaped struct |
| `include/zigbee_report.h` | Update + start API |
| `src/zigbee_report.cpp` | ED task; no wait-for-sensor gate |
| `src/main.cpp` | Fake sine readings → report |
| `README.md` | Flash + join checklist |

---

### Task 1: Scaffold project + config

- [x] Create `ul212-dummy-zigbee/` with `platformio.ini` (`esp32-c6-devkitc-1`, USB CDC, Zigbee ED, `lib_extra_dirs=../lib`)
- [x] Copy `partitions_zigbee.csv` from `ul212-rs232-fetch`
- [x] Add `zigbee_app_config.h` with dummy IDs / EPAN / metric map
- [x] `pio run -e esp32-c6-devkitc-1` may fail until sources exist — ok

### Task 2: Zigbee report path (no sensor wait)

- [x] Add `dummy_reading.h` with fields matching RS-232 `Ul212Reading`
- [x] Port `zigbee_report.cpp` from rs232: start ED immediately after first valid update (main provides one before/at start); send TLV 16–21 every 5 s when joined
- [x] Build succeeds

### Task 3: Synthetic main loop + README

- [x] `main.cpp`: generate height/smooth/temp/signal/tilt; call `zigbeeReportUpdate` each loop
- [x] README: build/flash, EPAN match, `fleet hosts` check
- [x] `pio run -e esp32-c6-devkitc-1` green
- [ ] Flash Super Mini if USB present; verify join logs (optional HW)

### Task 4: Spec status + commit (if user asks)

- [ ] Mark design status Implemented
- [ ] Commit only when user requests
