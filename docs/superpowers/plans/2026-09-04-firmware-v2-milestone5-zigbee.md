# Firmware v2 Milestone 5 (Zigbee + dynamic hosts → 1088) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** ESP32-C6 Zigbee coordinator; dynamic HELLO/REPORT hosts; uplink schema **1088** with host-owned identity and flat METRIC_MAP keys.

**Architecture:** Port/adapt `transport_zigbee` + `host_registry` + `fleet_protocol`; host-testable `uplink_host` builder; tick/once POST with host ids (not carrier).

**Spec:** `docs/superpowers/specs/2026-09-04-firmware-v2-milestone5-zigbee-design.md`

## Global Constraints

- Branch: `firmware-v2`; tree: `firmware_v2/` (+ `tests/host`)
- Do not edit legacy `components/`
- Channel mandatory (`CONFIG_FLEET_ZIGBEE_CHANNEL`, default 15)
- Vehicle wrap POST pattern from M3

## File map

| File | Role |
|------|------|
| `firmware_v2/master/components/transport_zigbee/` | Coordinator + radio stubs |
| `firmware_v2/master/components/host_registry/` | Dynamic registry |
| `firmware_v2/master/components/fleet_protocol/` | TLV codec |
| `firmware_v2/master/components/uplink/uplink_host.*` | 1088 payload builder |
| `firmware_v2/master/components/uplink/uplink.c` | `do_host_tick` / once |
| `firmware_v2/master/main/main.c` | Zigbee init after poller |
| `tests/host/test_uplink_host_v2.c` | Host test |

---

### Task 1: Zigbee transport CMake + Kconfig defaults

- [x] `CMakeLists.txt` links `esp-zigbee-lib` when `CONFIG_FLEET_ZIGBEE_ENABLE`
- [x] sdkconfig.defaults: Zigbee enable, channel 15, `CONFIG_ZB_ENABLED` / `CONFIG_ZB_ZCZR`

### Task 2: Uplink 1088

- [x] `UPLINK_SCHEMA_HOST`; `uplink_build_host_report_payload` (flat keys + optional `key_unit`)
- [x] `do_host_tick`: snapshot → POST per host with ≥1 valid reading and non-empty ids
- [x] Host test `test_uplink_host_v2`

### Task 3: Wire main + CLI + docs

- [x] `transport_zigbee_init/start` after poller (non-fatal warn)
- [x] CLI `fleet hosts` + help; VERSION 1.0.15; MASTER/README/HOW_TO_MAKE_A_HOST

### Task 4: Build + host tests

- [x] `idf.py build` green
- [x] Host host-payload builder test passes

### Follow-ups

M6 SD store-on-fail.
