# Firmware v2 — Milestone 3 Design (Uplink core + GPS 1089)

**Date:** 2026-09-04  
**Status:** Approved (roadmap reorder plan)  
**Branch:** `firmware-v2`  
**Tree:** `firmware_v2/master/`

## Goal

Land a shared **JSON uplink** module that builds the fleet envelope and POSTs over existing LTE QHTTP. First producer: **schema 1089 (GPS)** from M2 `lte_gps_get` / `lte_time_now_ms`. No SD, OBD, or Zigbee in this milestone.

## Decisions (locked)

| Topic | Choice |
|-------|--------|
| Envelope | `{ device_id, node_id, schemaId, ts_ms, payload }` — same as v1 / backend guide |
| Mandatory | All five top-level fields required; block POST if carrier `device_id`/`node_id` empty |
| Dynamic payload | `payload` is a JSON object; keys depend on schema (M3 only builds GPS keys) |
| POST URL | Default `https://api.trafyn.info/nc-events-api/v2/messages`; NVS override later OK |
| Body shape | Single object when one event (M3 always ≤1); array reserved for multi-event later |
| Auth | None required (match v1); optional headers deferred |
| GPS ids | Virtual: `{master_device_id}_GPS` / `node-{master_device_id}_GPS` (derived from NVS/USB master id only; not OTA-writable) |
| GPS gates | Send when `gps_ok` and worth: first fix **or** ≥50 m move **or** 5 min heartbeat |
| Bus | No full telemetry_bus yet — uplink task polls GPS directly (thin path) |
| Pins | Unchanged |

## Non-goals (M3)

- Schemas 1087 / 1088 producers  
- SD enqueue/drain  
- SoftAP  
- Editing legacy `components/` (reference only; may copy pure helpers)

## Architecture

```
lte_gps_get / lte_time_now_ms
        │
        ▼
  uplink_tick (periodic + CLI force)
        │
        ├─ build envelope + GPS payload (host-testable builders)
        └─ lte_http_post(url, body) → 2xx = success
```

## Module layout

| Path | Role |
|------|------|
| `components/uplink/` | ESP-IDF component: init, NVS identity, tick task, POST |
| `uplink_envelope.c/h` | Pure builders (no FreeRTOS): event JSON, GPS payload, virtual ids, worth_sending |
| `uplink.c/h` | Runtime: NVS, tick, status |
| CLI | `node_id`, `uplink status`, `uplink once` |
| Kconfig | Default URL, tick interval |

## Config / NVS

- Carrier `device_id` — reuse OTA cloud NVS key if already shared, or uplink namespace `uplink` keys `did` / `nid`
- `node_id` — USB `node_id <id>`; default empty → uplink blocked until set
- Default URL in Kconfig / sdkconfig.defaults

Align with existing CLI `device_id` used by OTA: prefer **one** carrier identity. M3: `device_id` command already persists OTA `device_id`; add `node_id` and sync uplink from the same provisioning story (read OTA device_id or shared NVS).

**Provision rule:** uplink uses `device_id` from OTA config (`fleet-demo-001` default) and requires `node_id` via new CLI before POST.

## GPS payload (1089)

Live POST body is always a JSON **array** of bare envelopes (even one event). No `Vehicle` wrap:

```json
[
  {
    "device_id": "fleet-demo-001_GPS",
    "node_id": "node-fleet-demo-001_GPS",
    "schemaId": "1089",
    "ts_ms": 1710000001000,
    "payload": {
      "gps_ok": true,
      "lat": 12.9716,
      "lng": 77.5946
    }
  }
]
```

Lab mock: `tools/telemetry_mock/` (ngrok → set `CONFIG_UPLINK_URL`). Schema-registry errors on Trafyn are separate from this body shape.

## CLI

- `node_id <id>` / show in `status`
- `uplink status` — enabled, last HTTP, last error, last send ts, queued flags (none in M3)
- `uplink once` — force one tick (ignore heartbeat gate if force flag; still require gps_ok unless testing)

## Success criteria

1. Build/flash; set `node_id`; with GPS fix, periodic or `uplink once` POSTs 1089 and gets HTTP 2xx (or logged status).
2. Without `node_id`, uplink refuses to POST.
3. Host unit tests for envelope / GPS payload / virtual ids / worth_sending.
4. No publish-multipart URL; no pin remaps; no legacy `components/` edits.
5. OTA + GPS CLI still work.

## Follow-ups

M4 plugs 1087 into the same envelope builders; M5 plugs 1088; M6 stores failed POST bodies.
