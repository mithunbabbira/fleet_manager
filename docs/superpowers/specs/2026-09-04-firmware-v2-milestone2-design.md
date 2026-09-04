# Firmware v2 — Milestone 2 Design (GPS + time)

**Date:** 2026-09-04  
**Status:** Approved for implementation  
**Branch:** `firmware-v2`  
**Tree:** `firmware_v2/master/` (old `components/` remains reference only)

## Goal

Enable Quectel EC200U GNSS and wall-clock sync already present in the `lte` module, and expose them on the USB CLI. Keep the change small and reliable — no new modules, no uplink yet.

## Decisions (locked)

| Topic | Choice |
|-------|--------|
| Packaging | Turn on existing `lte` GPS/time (approach 1) |
| Product surface | Enable + USB expose only (no NVS last-fix, no telemetry bus) |
| Time priority | CCLK first; GPS UTC from `QGPSLOC` as backup |
| Lab bar | Time must work indoors; outdoor GPS fix is nice-to-have |
| Pins | Unchanged — LTE UART GPIO16/17; GNSS is on-modem |

## Non-goals (M2)

- NVS persistence of last fix / last sync
- Telemetry bus publish or JSON schema `1089` uplink
- SoftAP, SD queue, OBD, Zigbee
- New `gps` / `time` ESP-IDF components
- Editing legacy root `components/`

## Architecture

```
lte_start()
  ├─ UART / modem bring-up
  ├─ CCLK sync on maintain path → lte_time_* cache
  └─ gps_task (CONFIG_LTE_GPS_ENABLE)
       ├─ AT+QGPS=1 with backoff
       └─ poll AT+QGPSLOC=2 → lat/lng cache + optional GPS time

cli status / gps  →  lte_time_get() + lte_gps_get()
```

OTA continues to call `lte_suspend_bg_at` so the GPS background task does not contend for the AT UART during firmware check/download.

## Config

In `firmware_v2/master/sdkconfig.defaults`:

- `CONFIG_LTE_GPS_ENABLE=y`
- Keep existing `CONFIG_LTE_GPS_MAX_AGE_S` (age gate for `lte_gps_get`)

No new Kconfig symbols required unless defaults prove wrong in lab.

## CLI

Extend `status`:

- `time: ok=… src=cclk|gps|none utc_ms=… ist=YYYY-MM-DD HH:MM:SS`
- `gps: ok=… lat=… lng=… age_ms=…`

Add command `gps` — print the GPS line only.

Existing OTA/config commands unchanged.

## Behaviour details

- IST formatting via `lte_format_ist` (India product; CCLK `+22` / IST assumption unchanged).
- Indoor: `gps_ok=no` is acceptable; `time_ok=yes` with `src=cclk` is required after modem register.
- Outdoor (optional): `gps_ok=yes` with plausible lat/lng and bounded `age_ms`.

## Success criteria

1. Flash M2 build; after LTE up, USB `status` shows `time_ok=yes` (usually `cclk`).
2. `status` / `gps` never crash; indoor no-fix is clean (`gps_ok=no`).
3. Optional: near window/outside, `gps_ok=yes` with sensible coordinates.
4. Smoke: `ota check` still completes with GPS enabled (suspend/resume path).
5. No publish-multipart URL in firmware; no pin remaps; no edits under legacy `components/`.

## Out of scope follow-ups (later milestones)

GPS/time on uplink envelope (`1089`), SD queue timestamps, host-side time — deferred.
