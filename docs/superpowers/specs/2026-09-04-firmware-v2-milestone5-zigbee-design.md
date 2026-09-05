# Firmware v2 — Milestone 5 Design (Zigbee + dynamic hosts → 1088)

**Date:** 2026-09-04  
**Status:** Locked scope (implement after M4)  
**Depends on:** M3 uplink envelope

## Goal

ESP32-C6 native Zigbee coordinator; channel **mandatory** in Kconfig; dynamic hosts via HELLO/REPORT TLV; uplink schema **1088** with host-owned identity and dynamic payload keys from METRIC_MAP.

## Config

- `CONFIG_FLEET_ZIGBEE_CHANNEL` (default 15) — master and hosts must match
- Document in `firmware_v2/host/HOW_TO_MAKE_A_HOST.md`

## Scope

- Coordinator bring-up; open network; cluster 0xFC00
- Registry without required static catalog (dynamic hosts)
- One 1088 event per host with ≥1 valid reading; flat metric keys on `payload`

## Non-goals

SD, SoftAP, deleting legacy hosts.

## Success

UL212 (or lab host) joins configured channel; cloud receives 1088 with host `device_id` / `node_id` / `schemaId` from HELLO.
