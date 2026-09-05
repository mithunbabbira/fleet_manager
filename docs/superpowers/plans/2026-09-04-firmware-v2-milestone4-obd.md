# Firmware v2 Milestone 4 (MCP2515 / OBD → 1087) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Soft-SPI MCP2515 on frozen pins; Mode-01 poll → schema **1087** via M3 uplink (keep GPS **1089**).

**Architecture:** Port `can_obd` + `obd_codec`; thin `obd_poller`; host-testable `uplink_obd` builder; tick/once POST with carrier ids.

**Spec:** `docs/superpowers/specs/2026-09-04-firmware-v2-milestone4-obd-design.md`

## Global Constraints

- Branch: `firmware-v2`; tree: `firmware_v2/` (+ `tests/host`)
- Do not edit legacy `components/`
- Pins: SCK=21 MOSI=22 MISO=23 CS=20 INT=14 (`board_pins.h`)
- Vehicle wrap POST pattern from M3

## File map

| File | Role |
|------|------|
| `firmware_v2/master/components/can_obd/` | MCP2515 + ISO-TP + link detect |
| `firmware_v2/master/components/obd_codec/` | Mode-01 decode |
| `firmware_v2/master/components/obd_poller/` | Thin 010C/010D/0105/0111 poller |
| `firmware_v2/master/components/uplink/uplink_obd.*` | 1087 payload builder |
| `firmware_v2/master/components/uplink/uplink.c` | Tick/once POST 1087 + 1089 |
| `firmware_v2/master/main/main.c` | can_obd + poller boot |
| `tests/host/test_uplink_obd_v2.c` | Host test |

---

### Task 1: Port can_obd + obd_codec

- [x] Copy into `firmware_v2/master/components/`; pins via `BOARD_MCP_*`
- [x] Kconfig defaults match PCB pins

### Task 2: Thin obd_poller

- [x] Hardcoded Mode-01 cmds; snapshot with pid views; fresh ≤15 s helper

### Task 3: Uplink 1087

- [x] `UPLINK_SCHEMA_OBD`; `uplink_build_obd_payload`; tick/once POST with carrier ids
- [x] Host test `test_uplink_obd_v2`

### Task 4: Wire main + CLI + docs

- [x] MCP CS idle-high; `can_obd_init/start`; `obd_poller_init/start`
- [x] `can status` + status `obd:` line; VERSION 1.0.14; MASTER/README

### Task 5: Build + host tests

- [x] `idf.py build` green
- [x] Host OBD builder test passes

### Follow-ups

M5 Zigbee 1088 / M6 SD store-on-fail.
