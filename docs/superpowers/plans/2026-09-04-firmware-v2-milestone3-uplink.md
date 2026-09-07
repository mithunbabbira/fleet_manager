# Firmware v2 Milestone 3 (Uplink + GPS 1089) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Shared JSON uplink module with mandatory envelope fields, dynamic `payload`, POST via `lte_http_post`, and first producer schema **1089** from M2 GPS/time.

**Architecture:** Host-testable builders in `uplink_envelope.c`; runtime tick/NVS/POST in `uplink.c`; CLI `node_id` / `uplink status` / `uplink once`. No SD, OBD, or Zigbee.

**Tech Stack:** ESP-IDF, ESP32-C6, Quectel EC200U QHTTP, Trafyn `nc-events-api/v2/messages`.

**Spec:** `docs/superpowers/specs/2026-09-04-firmware-v2-milestone3-uplink-design.md`

## Global Constraints

- Branch: `firmware-v2`; tree: `firmware_v2/` (+ `tests/host` for builders)
- Do not edit legacy `components/`
- Pins frozen; no publish-multipart URL in firmware
- POST body wrapped as `{"Vehicle":<envelope>}` for current Trafyn API

## File map

| File | Role |
|------|------|
| `firmware_v2/master/components/uplink/` | Envelope builders + tick/POST |
| `firmware_v2/master/components/cli/cli.c` | `node_id`, `uplink *` |
| `firmware_v2/master/main/main.c` | `uplink_init` / `uplink_start` |
| `tests/host/test_uplink_envelope_v2.c` | Host tests |

---

### Task 1: Envelope builders + host tests

- [x] Pure helpers: virtual GPS ids, distance/worth_sending, GPS payload, envelope
- [x] Host test `test_uplink_envelope_v2` passes

### Task 2: Runtime uplink + CLI

- [x] NVS `node_id`; tick task; `uplink_once`; Vehicle wrap; Kconfig URL/tick
- [x] Wire main + CLI; VERSION 1.0.13

### Task 3: Device verify

- [x] Build/flash; provision `node_id`; with GPS, POST 1089 logged
- [x] Lab: HTTP 400 from schema-registry is backend-side; device produce path OK

### Follow-ups (other milestones)

M4 1087 / M5 1088 / M6 SD store-on-fail.
