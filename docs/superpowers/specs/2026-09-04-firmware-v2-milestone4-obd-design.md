# Firmware v2 — Milestone 4 Design (MCP2515 / OBD → 1087)

**Date:** 2026-09-04  
**Status:** Locked scope (implement after M3)  
**Depends on:** M3 uplink envelope + POST

## Goal

Soft-SPI MCP2515 on frozen pins; OBD poll → schema **1087** events via M3 uplink.

## Pins (frozen)

| Signal | GPIO |
|--------|------|
| SCK | 21 |
| MOSI | 22 |
| MISO | 23 |
| CS | 20 |
| INT | 14 (reserved; poll OK) |

## Scope

- Port/simplify `can_obd` + thin poller into `firmware_v2/master/components/`
- Publish PIDs into uplink snapshot; build 1087 payload (v1 field set: profile, rpm, speed, … when fresh ≤15 s)
- Identity: carrier `device_id` / `node_id` from M3 provisioning

## Non-goals

Zigbee, SD, SoftAP, full legacy SoftAP console.

## Success

CAN detect on bench or vehicle; `uplink once` / tick includes 1087 when ≥1 fresh PID.
