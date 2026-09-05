# Firmware v2 Milestone 6 (SD store-on-fail) Implementation Plan

> **For agentic workers:** Use executing-plans / subagent-driven-development.

**Goal:** Mount microSD on SPI2; enqueue failed uplink envelopes; drain batch POST when LTE recovers.

**Architecture:** `store_sd` FatFS NDJSON queue; uplink `post_vehicle_envelope` enqueues bare envelope on fail; drain task peeks → Vehicle-wrap → POST → ack.

**Spec:** `docs/superpowers/specs/2026-09-04-firmware-v2-milestone6-sd-design.md`

## Tasks

- [x] Port `store_sd` into `firmware_v2/master/components/store_sd`
- [x] Pure `uplink_queue_*` helpers + host test
- [x] Store-on-fail in `post_vehicle_envelope`; drain task
- [x] Boot: SPI CS idle-high (SD+MCP), `store_sd_init` non-fatal
- [x] CLI `sd status`; uplink status queue counters
- [x] sdkconfig defaults for SD pins 4/5/6/18
