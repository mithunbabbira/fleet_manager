# LTE Telemetry Cloud Uplink — Design Spec

**Date:** 2026-07-24  
**Status:** Implemented on MCP2515 (not BLE ELM). UART pins: printed PCB GPIO16 TX / GPIO17 RX.  
**Platform:** ESP32-C6 Mini + Quectel EC200U (UART1 GPIO16 TX / GPIO17 RX on the printed PCB)  
**Related:** `PROJECT_CONTEXT.md`, `docs/sample-obd-telemetry.md`, `net_lte` Phase-1 AT path

## 1. Goal

When the ESP32-C6 has a live OBD-II session (BLE ELM connected, ELM ready, poller on) and cloud uplink is enabled from the SoftAP UI:

1. Periodically POST a JSON event to  
   `https://api.trafyn.info/nc-events-api/v2/messages`  
   over the **LTE modem** (Quectel HTTP AT — no PPP).
2. Show last uplink success/failure (and skip reason) on the SoftAP web page.
3. Let the operator set enable flag, POST interval, `device_id`, and `node_id` on the SoftAP page; persist them in **NVS**.
4. Never present misleading PID values (e.g. speed **255**) when data is missing or invalid — SoftAP shows `-`, API body uses `null` + `*_ok: false`.

### Non-goals

- PPP / ESP32 native TCP/TLS stack for LTE.
- Auth headers / API keys (caller curl has none).
- Changing the cloud schema beyond the agreed payload fields.
- Zigbee / fleet UART uplink (unchanged).

## 2. Decisions (locked)

| Topic | Decision |
|-------|----------|
| Transport | Quectel HTTP AT over existing `net_lte` UART + PDP (`QIACT`) |
| Enable model | SoftAP toggle, **default off**; when on, gate with live OBD rule |
| Live OBD gate | `ble_connected && elm_ready && poller_on && ≥1 fresh ok sample` (age ≤ 15 s) |
| Interval | Default **5 s**, NVS, clamp **1–300** |
| Missing PIDs | JSON `null` + `*_ok: false` (never invent fake values like speed 255; legitimate `0` with `ok=true` is allowed) |
| Identity | Hardcoded `schemaId: "1087"`; `device_id` / `node_id` editable + NVS |
| SoftAP feedback | Last attempt: ok, HTTP status, skip reason, timestamp, error text |
| Architecture | New `telemetry_uplink` component; does not talk to BLE/ELM directly |

## 3. Architecture & data flow

```
obd_poller → telemetry_bus → uplink sample cache (subscriber)
sys_runtime / ble_elm / elm327_client / profile_store / bond
        ↓
telemetry_uplink task (interval from NVS)
        ↓  (if enabled + gate pass)
build JSON body (schemaId + payload)
        ↓
net_lte_http_post(url, body)  // Quectel QHTTP* under UART mutex
        ↓
https://api.trafyn.info/nc-events-api/v2/messages
        ↓
last_result → SoftAP GET /api/uplink (+ UI card)
```

### Component boundaries

| Unit | Responsibility | Depends on |
|------|----------------|------------|
| `telemetry_uplink` | NVS config, sample cache, gating, JSON build, interval/`send now` task, last_result | `telemetry_bus`, `sys_runtime`, `ble_elm`, `elm327_client`, `obd_poller`, `profile_store`, `net_lte`, NVS/cJSON |
| `net_lte` HTTP helper | PDP ensure + Quectel HTTPS POST; shared AT mutex | UART driver, existing `net_lte` status |
| `transport_http` | `/api/uplink*` + SoftAP “Cloud uplink” card | `telemetry_uplink` |
| `obd_codec` / poller publish | Mark invalid/sentinel PID samples `ok=false` | — |

## 4. Configuration (NVS)

Namespace: `elm` (same as profiles/bond). Keys:

| Key | Type | Default |
|-----|------|---------|
| `uplink_enabled` | bool/u8 | `0` (false) |
| `uplink_interval_s` | u16 | `5` |
| `uplink_device_id` | string | `fleet-demo-001` |
| `uplink_node_id` | string | `esp32c6-01` |

Hardcoded (not NVS):

- URL: `https://api.trafyn.info/nc-events-api/v2/messages`
- `schemaId`: `"1087"`
- `schema_version`: `1`
- `source`: `"esp32_obd"`

Validation on save: interval clamped to 1–300; empty `device_id` / `node_id` rejected.

## 5. Payload shape

Matches the operator’s curl example:

```json
{
  "schemaId": "1087",
  "payload": {
    "device_id": "...",
    "node_id": "...",
    "schema_version": 1,
    "ble_peer_address": "...",
    "adapter_name": "...",
    "obd_profile": "...",
    "obd_protocol": "...",
    "uptime_seconds": 0,
    "ble_connected": true,
    "elm_ready": true,
    "poller_status": "on",
    "cmds_ok": 0,
    "cmds_fail": 0,
    "ble_reconnects": 0,
    "blocked_cmds": 0,
    "telemetry_drops": 0,
    "rpm": null,
    "speed_kmh": null,
    "coolant_c": null,
    "throttle_pct": null,
    "voltage_v": null,
    "rpm_raw_hex": null,
    "speed_raw_hex": null,
    "coolant_raw_hex": null,
    "throttle_raw_hex": null,
    "voltage_raw": null,
    "rpm_age_ms": null,
    "speed_age_ms": null,
    "coolant_age_ms": null,
    "throttle_age_ms": null,
    "voltage_age_ms": null,
    "rpm_ok": false,
    "speed_ok": false,
    "coolant_ok": false,
    "throttle_ok": false,
    "voltage_ok": false,
    "source": "esp32_obd"
  }
}
```

Rules:

- Per-PID freshness window: **15 s** (same as gate). If sample missing, not `ok`, or `age_ms > 15000` → value/raw/age are JSON `null` and `*_ok` is `false`.
- Never send fabricated numeric defaults (including speed `255`). A real parked speed of `0` with `ok=true` is valid.
- Status/metrics fields always filled from live system state at POST time.
- `obd_protocol`: use last cached `ATDP` text if present; else `"unknown"`.

## 6. SoftAP UI & REST

### UI card “Cloud uplink” (below LTE card)

- Enable checkbox (persists on Save / toggle apply)
- Interval (seconds), `device_id`, `node_id` + Save
- Status: last ok/fail, HTTP status, skip reason, relative time, short error
- Button: **Send now** (same gate + path as interval tick)

Live telemetry widgets already show `-` when `!sample.ok`; reinforce that invalid speed never appears as `255`.

### REST

| Method | Path | Behavior |
|--------|------|----------|
| GET | `/api/uplink` | Config + `last` result object |
| POST | `/api/uplink` | Update `enabled` / `interval_s` / `device_id` / `node_id` → NVS |
| POST | `/api/uplink/send` | Force one attempt; return attempt result |

`last` object fields: `ok`, `http_status`, `skipped` (bool), `reason`, `ts_ms`, `error`.

## 7. LTE HTTP helper (`net_lte`)

Under the existing UART AT mutex:

1. Ensure PDP: `QICSGP` with configured APN + `QIACT=1` (reuse selftest patterns).
2. Configure HTTP context (`QHTTPCFG` context id; SSL/SNI as required for HTTPS host).
3. Set URL (`QHTTPURL`).
4. POST body (`QHTTPPOST`) with `Content-Type: application/json`.
5. Parse HTTP status from modem response / `QHTTPREAD`; **2xx = success**.
6. Return structured result to caller; update `net_lte` IP/link flags when PDP/IP known.

Concurrency: uplink, serial `lte`/`lte test`, and HTTP `/api/lte*` must serialize on the same mutex (timeout → clear error, no deadlock).

## 8. Misleading speed (255) fix

Root cause class: invalid or sentinel OBD bytes (e.g. `0xFF`) or failed decode still exposed as a numeric “value”.

Requirements:

- Codec / poller: do not publish `ok=true` for invalid speed (and similarly guard other PID decodes where sentinel values are meaningless).
- SoftAP `setLive`: keep `-` when `!ok` (already present); do not fall back to raw bogus numbers.
- Uplink builder: only emit numeric PID fields when `ok` and fresh; else `null` + `*_ok: false`.

## 9. Boot / lifecycle

In `app_main`, after `telemetry_bus_init` and `net_lte_start` (and other existing inits):

- `telemetry_uplink_start()` — loads NVS, subscribes to bus, spawns interval task.
- Non-fatal if LTE disabled: uplink reports skip/`LTE unavailable` without blocking SoftAP/BLE.

## 10. Error handling

- Gate fail → skip POST; SoftAP shows reason (`disabled`, `not ready`, `no fresh sample`, `lte busy`, etc.).
- Modem/HTTP fail → store error + `ok=false`; retry on next interval.
- Never crash the poller or SoftAP on uplink failure.
- JSON build failure → skip send with reason.

## 11. Testing

- Host unit test: JSON builder — missing sample → null/`ok:false`; valid sample → numbers; speed invalid never becomes 255.
- On-device: enable uplink on SoftAP; with OBD live + LTE internet (already verified via `lte test`), confirm POST success on UI and backend.
- On-device: disable OBD / disconnect BLE → ticks skip with reason, no misleading speed on UI.
- Interval change persists across reboot (NVS).

## 12. Implementation notes

- Prefer a focused `telemetry_uplink` component over stuffing logic into `transport_http` or `net_lte`.
- Keep SoftAP HTML changes in `static_index.html.h` consistent with existing card style.
- Update `PROJECT_CONTEXT.md` after implementation (LTE uplink status).
