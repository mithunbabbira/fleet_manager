# Firmware v2 — Milestone 6 Design (SD store-on-fail)

**Date:** 2026-09-04  
**Status:** Implemented (firmware_v2)  
**Depends on:** M3+ uplink produce path (failed POST bodies)

## Goal

Mount microSD on frozen SPI2 pins; enqueue failed uplink JSON lines; drain batch POST when LTE recovers.

## Pins (frozen)

| Signal | GPIO |
|--------|------|
| SCK | 4 |
| MOSI | 5 |
| MISO | 6 |
| CS | 18 |

## Scope

- FatFS mount; NDJSON queue of bare envelopes (+ `queued_at_ms`)
- On POST fail → enqueue; drain task → Vehicle-wrapped POST → ack on 2xx
- SPI CS idle-high for SD + MCP at boot

## Non-goals

Changing envelope shape; SoftAP upload of bins.

## Success

Force LTE fail → lines on card → restore LTE → drain clears queue with 2xx (or logged HTTP status).
