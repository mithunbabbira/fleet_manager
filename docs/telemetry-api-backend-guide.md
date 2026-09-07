# Fleet telemetry API — backend integration guide

**Audience:** Backend / API developers consuming carrier uplink events  
**Firmware source of truth:** `firmware_v2/master/components/uplink/` (`uplink_envelope.c`, `uplink_obd.c`, `uplink_host.c`, `uplink_batch.c`)  
**Last updated:** 2026-09-05

---

## Endpoint

| Item | Value |
|------|--------|
| **Method** | `POST` |
| **URL (factory default)** | `https://api.trafyn.info/nc-events-api/v2/messages` |
| **Content-Type** | `application/json` |
| **Auth from device** | None today |
| **Transport** | Quectel EC200U LTE modem (HTTPS via QHTTP AT) |

The POST URL can be overridden per device in NVS (`uplink url <url>` on USB CLI). Schema IDs are compile-time (`UPLINK_SCHEMA_*` in uplink headers).

---

## Event envelope (all types)

Every item uses the same top-level envelope. Identity, schema, and time sit **outside** `payload`:

```json
{
  "device_id": "fleet-demo-001",
  "node_id": "node-fleet-demo-001",
  "schemaId": "1087",
  "ts_ms": 1710000001000,
  "payload": { }
}
```

### Mandatory envelope fields

| Field | Required | Notes |
|-------|----------|--------|
| `device_id` | **Yes** | Carrier id, virtual `{master}_GPS`, or host id (e.g. `ul212-rs232-001`) |
| `node_id` | **Yes** | Paired with `device_id` |
| `schemaId` | **Yes** | `1087` / `1088` / `1089` |
| `ts_ms` | **Yes** | **UTC** Unix epoch milliseconds when LTE/GPS time is synced; else device uptime ms. Convert to IST (`UTC+5:30`) only for display — do not store IST as a fake epoch. |
| `payload` | **Yes** | Domain data only — do **not** expect identity/time inside |

Uplink is blocked until the carrier is provisioned with `device_id` + `node_id` (NVS).

---

## Schema IDs

| schemaId | Type | `device_id` example | `node_id` example |
|----------|------|---------------------|-------------------|
| `1087` | OBD / vehicle telemetry | `fleet-demo-001` | `node-fleet-demo-001` |
| `1088` | Zigbee host snapshot (UL212) | `ul212-rs232-001` | `node-ul212-rs232-001` |
| `1089` | GNSS | `fleet-demo-001_GPS` | `node-fleet-demo-001_GPS` |

---

## Body shape (locked)

**Live POST and SD drain** send a JSON **array of bare envelopes**, optionally wrapped for Trafyn:

```json
{"Vehicle":[ { "device_id": "...", "node_id": "...", "schemaId": "1089", "ts_ms": 1, "payload": { } } ]}
```

| Build flag | Body |
|------------|------|
| `CONFIG_UPLINK_VEHICLE_WRAP=y` (default) | `{"Vehicle":[...]}` — required by current Trafyn API |
| `CONFIG_UPLINK_VEHICLE_WRAP=n` | bare `[...]` — lab mock (`tools/telemetry_mock`) |

Even one event is still an array inside `Vehicle` (or a bare length-1 array when wrap is off).

**Known Trafyn gap (2026-09-05):** after the Vehicle key is present, API may still return 400 with schema-registry `globalIds/null`. That is a backend/schema-registry issue, not a missing Vehicle key. Device will SD-queue failed POSTs until ingest succeeds.

Normalize on ingest if you still accept legacy single objects:

```javascript
const body = await request.json();
const events = Array.isArray(body) ? body : [body];
```

### What can appear in one tick

A single produce tick may emit **0..N** events (capped at 12) in **one** array:

| Event | Condition |
|-------|-----------|
| `1087` OBD | ≥1 fresh PID (≤15 s) |
| `1089` GPS | GNSS fix (`gps_ok`) **and** move/heartbeat gate (or forced) |
| `1088` host | One element **per** Zigbee host with ≥1 valid reading |

Examples:

- Truck + GPS + one UL212 → **3-element array** in one HTTP POST
- Bench, no CAN, GPS only → **1-element array** (`1089`)
- Lab without GPS → use device `uplink test` (synthetic 1089) or restore URL after mock

Return **HTTP 2xx** only on successful ingest. The device keeps SD queue lines until drain gets 2xx.

---

## Full sample body (list) — share with backend

Typical live/batch POST when the vehicle has OBD, GPS fix, and one UL212 fuel host:

```json
[
  {
    "device_id": "carrier-042",
    "node_id": "node-042",
    "schemaId": "1087",
    "ts_ms": 1710000001000,
    "payload": {
      "obd_profile": "fleet_basic",
      "obd_protocol": "ISO15765-4 CAN11/500",
      "uptime_seconds": 3605,
      "poller_status": "on",
      "cmds_ok": 1440,
      "cmds_fail": 3,
      "blocked_cmds": 0,
      "telemetry_drops": 0,
      "rpm": 790,
      "rpm_raw_hex": "410C0C31",
      "rpm_age_ms": 80,
      "rpm_ok": true,
      "speed_kmh": 0,
      "speed_raw_hex": "410D00",
      "speed_age_ms": 90,
      "speed_ok": true,
      "coolant_c": 84,
      "coolant_raw_hex": "41057C",
      "coolant_age_ms": 120,
      "coolant_ok": true,
      "throttle_pct": 12.5,
      "throttle_raw_hex": "411120",
      "throttle_age_ms": 100,
      "throttle_ok": true,
      "voltage_v": 13.8,
      "voltage_raw": "13.8",
      "voltage_age_ms": 200,
      "voltage_ok": true,
      "source": "esp32_obd"
    }
  },
  {
    "device_id": "fleet-demo-001_GPS",
    "node_id": "node-fleet-demo-001_GPS",
    "schemaId": "1089",
    "ts_ms": 1710000001000,
    "payload": {
      "gps_ok": true,
      "lat": 12.9716000,
      "lng": 77.5945600
    }
  },
  {
    "device_id": "ul212-001",
    "node_id": "node-ul212-001",
    "schemaId": "1088",
    "ts_ms": 1710000001000,
    "payload": {
      "host_type": "ul212_ble_fetch",
      "height_mm": 130.7,
      "height_mm_unit": "mm",
      "smooth_mm": 131.5,
      "smooth_mm_unit": "mm",
      "temperature_c": 33.2,
      "temperature_c_unit": "C",
      "signal": 90,
      "valid_echo": 1,
      "tilt_deg": 3
    }
  }
]
```

### Sample — one-event array (live POST with only GPS)

Same shape as multi-event: always an array of length 1.

```json
{
  "device_id": "ul212-001",
  "node_id": "node-ul212-001",
  "schemaId": "1088",
  "ts_ms": 1710000001100,
  "payload": {
    "host_type": "ul212_ble_fetch",
    "height_mm": 130.7,
    "height_mm_unit": "mm",
    "smooth_mm": 131.5,
    "smooth_mm_unit": "mm",
    "temperature_c": 33.2,
    "temperature_c_unit": "C",
    "signal": 90,
    "valid_echo": 1,
    "tilt_deg": 3
  }
}
```

---

## Sample — bench / no vehicle (sparse OBD)

When CAN is down, numeric PID fields are omitted; `*_ok` flags are still sent as `false`. An OBD event is only emitted if **at least one** PID is fresh — otherwise the tick may contain only GPS and/or host events.

```json
{
  "device_id": "carrier-bench-01",
  "node_id": "node-bench-01",
  "schemaId": "1087",
  "ts_ms": 1710000005000,
  "payload": {
    "obd_profile": "fleet_basic",
    "obd_protocol": "unknown",
    "uptime_seconds": 120,
    "poller_status": "paused",
    "cmds_ok": 0,
    "cmds_fail": 0,
    "blocked_cmds": 0,
    "telemetry_drops": 0,
    "rpm_ok": false,
    "speed_ok": false,
    "coolant_ok": false,
    "throttle_ok": false,
    "voltage_ok": false,
    "source": "esp32_obd"
  }
}
```

---

## Payload field reference

### Schema 1087 — OBD

| Field | Type | Notes |
|-------|------|--------|
| `obd_profile` | string | Active poll profile (e.g. `fleet_basic`) |
| `obd_protocol` | string | e.g. `ISO15765-4 CAN11/500` or `unknown` / `none` |
| `uptime_seconds` | uint32 | Carrier uptime |
| `poller_status` | string | `"on"` / `"paused"` (may also appear as running/paused in logs) |
| `cmds_ok`, `cmds_fail`, `blocked_cmds`, `telemetry_drops` | uint64 | Counters |
| `rpm_ok`, `speed_ok`, `coolant_ok`, `throttle_ok`, `voltage_ok` | bool | Always present |
| `rpm`, `speed_kmh`, `coolant_c`, `throttle_pct`, `voltage_v` | number | Only when matching `*_ok` is true (fresh ≤15 s) |
| `*_raw_hex` / `voltage_raw`, `*_age_ms` | string / uint | Only when that PID is fresh |
| `source` | string | Always `"esp32_obd"` |

GPS and host data are **not** nested in 1087.

### Schema 1089 — GPS

| Field | When |
|-------|------|
| `gps_ok` | Always |
| `lat`, `lng` | Only when `gps_ok == true` |

Virtual ids: `{master}_GPS` / `node-{master}_GPS` from the provisioned carrier `device_id` (not OTA-writable).

### Schema 1088 — Host snapshot (bundled) — **updated 2026-09-03**

**One event per Zigbee host** per tick. All **valid** readings are flat keys on `payload` (not one event per key).

| Field | Type | Notes |
|-------|------|--------|
| `host_type` | string | e.g. `ul212_ble_fetch` |
| `{key}` | number | e.g. `height_mm`, `tilt_deg` — only if that reading is valid |
| `{key}_unit` | string | Present only when the reading has a non-empty unit |

Host `device_id` / `node_id` / `schemaId` are **sent by the Zigbee host** (TLV envelope).
Carrier no longer invents `node-{device_id}` or maps `host_type_id → 1088` for uplink.
`ts_ms` = **carrier tick time** (shared time base for the batch).

A host quiet for >2 minutes is dropped from uplink so stale values are not re-sent.

### UL212 keys (`host_type_id: 1`)

| key | typical unit field |
|-----|--------------------|
| `height_mm` | `height_mm_unit`: `"mm"` |
| `smooth_mm` | `smooth_mm_unit`: `"mm"` |
| `temperature_c` | `temperature_c_unit`: `"C"` |
| `signal` | (no unit) |
| `valid_echo` | (no unit) |
| `tilt_deg` | (no unit) |

---

## Wall-clock time (`ts_ms`)

`ts_ms` is always a **UTC epoch** (milliseconds since 1970-01-01 UTC). The carrier gets wall clock from the EC200U 4G modem — no public HTTP time API:

| Priority | Source | Notes |
|----------|--------|--------|
| 1 | GNSS UTC | From `AT+QGPSLOC` when a fix is available (preferred) |
| 2 | `AT+CCLK?` | Network/NITZ time; India operators report IST as Quectel `+22` (UTC+5:30). If the TZ field is omitted, firmware assumes IST. |
| 3 | ESP uptime ms | Before first successful sync |

**India display:** `IST = UTC + 5:30`. Example: `ts_ms = 1773997200000` → `2026-03-20 14:30:00 IST`. Serial console `lte` prints both `utc_ms` and `ist="..."`.

Treat `ts_ms` below `1735689600000` (2025-01-01) as “clock not synced yet”; prefer receive time.

---

## Backend ingest checklist

1. Parse envelope: `device_id`, `node_id`, `schemaId`, `ts_ms`, `payload`.
2. Normalize body: `Array.isArray(body) ? body : [body]`.
3. Route by `schemaId`: `1087` / `1088` / `1089`.
4. Index OBD by `(device_id, ts_ms)`.
5. Index GPS by virtual `device_id` (or correlate carrier via suffix).
6. Index host by `(device_id, ts_ms)` — metrics are **columns/keys on payload**, not `payload.key`.
7. Keep namespaces separate: carrier ≠ `gps-*` ≠ `ul212-*`.

---

## Breaking change vs earlier 1088 format

| Before (early Sep 2026) | Now (2026-09-03) |
|-------------------------|------------------|
| One `1088` event **per reading** | One `1088` event **per host** |
| `payload: { key, value, unit, valid }` | `payload: { host_type, height_mm, tilt_deg, … }` |
| Six UL212 metrics → six events | Six valid metrics → **one** event |

Do **not** expect `payload.key` / `payload.value` for new firmware.

---

## Related repo paths

| Path | Purpose |
|------|---------|
| `firmware_v2/master/components/uplink/uplink_envelope.c` | Envelope + GPS payload |
| `firmware_v2/master/components/uplink/uplink_obd.c` | OBD 1087 payload |
| `firmware_v2/master/components/uplink/uplink_host.c` | Host 1088 payload |
| `firmware_v2/master/components/uplink/uplink_batch.c` | Multi-envelope → one JSON array |
| `docs/fleet-zigbee-host-guide.md` | Zigbee host onboarding |
| `tools/carrier_console/` | PC provisioning UI |

---

## Quick curl test (array)

```bash
curl -sS -X POST 'https://api.trafyn.info/nc-events-api/v2/messages' \
  -H 'Content-Type: application/json' \
  -d @- <<'EOF'
{
  "device_id": "ul212-001",
  "node_id": "node-ul212-001",
  "schemaId": "1088",
  "ts_ms": 1710000001100,
  "payload": {
    "host_type": "ul212_ble_fetch",
    "height_mm": 130.7,
    "height_mm_unit": "mm",
    "smooth_mm": 131.5,
    "smooth_mm_unit": "mm",
    "temperature_c": 33.2,
    "temperature_c_unit": "C",
    "signal": 90,
    "valid_echo": 1,
    "tilt_deg": 3
  }
}
EOF
```
