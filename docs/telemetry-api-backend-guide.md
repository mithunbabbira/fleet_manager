# Fleet telemetry API — backend integration guide

**Audience:** Backend / API developers consuming carrier uplink events  
**Firmware source of truth:** `components/telemetry_uplink/uplink_payload.c`, `uplink_schema.h`  
**Last updated:** 2026-09-02

---

## Endpoint

| Item | Value |
|------|--------|
| **Method** | `POST` |
| **URL (factory default)** | `https://api.trafyn.info/nc-events-api/v2/messages` |
| **Content-Type** | `application/json` |
| **Auth from device** | None today |
| **Transport** | Quectel EC200U LTE modem (HTTPS via QHTTP AT) |

The POST URL can be overridden per device in NVS (Carrier Console / serial). OBD `schemaId` defaults to `1087` and can also be overridden in NVS (`uplink schema`). Host and GPS schema IDs are defined in firmware (`uplink_schema.h`).

---

## Event envelope (all types)

Each uplink item uses the same top-level envelope. Identity, schema, and time sit **outside** `payload`:

```json
{
  "device_id": "carrier-042",
  "node_id": "node-042",
  "schemaId": "1087",
  "ts_ms": 1710000001000,
  "payload": { ... }
}
```

### Mandatory envelope fields (every event)

| Field | Required | Notes |
|-------|----------|--------|
| `device_id` | **Yes** | Non-empty string; carrier, virtual `gps-*`, or host id |
| `node_id` | **Yes** | Non-empty string; paired with `device_id` |
| `schemaId` | **Yes** | Non-empty; `1087` / `1088` / `1089` (see `uplink_schema_ids.h`) |
| `ts_ms` | **Yes** | Always present; UTC epoch ms when LTE time synced, else uptime ms |
| `payload` | **Yes** | Domain data only — identity/time are **not** duplicated inside |

Firmware rejects building or serializing an event if any of `device_id`, `node_id`, or `schemaId` is missing/empty. Uplink is blocked until carrier `device_id` + `node_id` are provisioned (NVS).

| Field | Description |
|-------|-------------|
| `device_id` | Event source identity (carrier, virtual GPS device, or sensor host) |
| `node_id` | Secondary node identity for the same event source |
| `schemaId` | Trafyn schema — determines `payload` shape |
| `ts_ms` | Capture time — **UTC epoch ms** when LTE time is synced; otherwise device uptime ms |
| `payload` | Domain-specific fields only |

---

## Schema IDs (firmware defaults)

| schemaId | Type | `device_id` example |
|----------|------|---------------------|
| `1087` | OBD / vehicle telemetry | `carrier-042` (provisioned) |
| `1088` | Host sensor reading (UL212, `host_type_id` 1) | `ul212-001` |
| `1089` | GNSS position | `gps-042` (virtual, derived from carrier) |

Schema map source: `components/telemetry_uplink/uplink_schema.c`. **Edit schema numbers in `components/telemetry_uplink/include/uplink_schema_ids.h` before build/OTA.**

---

## Request body shapes

### Live POST (no SD card)

- **One event** → single JSON object (envelope above)
- **Multiple events** from one tick → JSON array of envelopes

### Batch POST (SD queue drain)

Always a JSON array of envelopes (one SD queue line per event):

```json
[
  { "device_id": "carrier-042", "node_id": "node-042", "schemaId": "1087", "ts_ms": ..., "payload": { ... } },
  { "device_id": "gps-042", "node_id": "node-gps-042", "schemaId": "1089", "ts_ms": ..., "payload": { ... } },
  { "device_id": "ul212-001", "node_id": "node-ul212-001", "schemaId": "1088", "ts_ms": ..., "payload": { ... } }
]
```

**Parsing rule:**

```javascript
const body = await request.json();
const events = Array.isArray(body) ? body : [body];
```

Return **HTTP 2xx** only on successful ingest. The device keeps queued SD lines until drain gets 2xx.

---

## Sample — one uplink tick (OBD + GPS + host reading)

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
      "rpm": 790.0,
      "rpm_raw_hex": "410C0C31",
      "rpm_age_ms": 80,
      "rpm_ok": true,
      "speed_ok": false,
      "coolant_ok": false,
      "throttle_ok": false,
      "voltage_ok": false,
      "source": "esp32_obd"
    }
  },
  {
    "device_id": "gps-042",
    "node_id": "node-gps-042",
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
      "key": "height_mm",
      "value": 40.5,
      "unit": "mm",
      "valid": true
    }
  }
]
```

Each valid host reading emits its own `1088` event. A UL212 reporting six valid readings can produce six events in the same tick.

---

## Sample — bench / no vehicle (OBD event with no fresh PIDs)

When no CAN ECU is connected, the carrier may still emit an OBD-schema event with all `*_ok: false` and no numeric PID fields:

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

Note: OBD events are only emitted when at least one PID is fresh (≤15 s). Bench units with no CAN may emit GPS or host events only.

---

## Payload field reference

### Schema 1087 — OBD (`payload`)

| Field | Type | Notes |
|-------|------|--------|
| `obd_profile` | string | Active poll profile |
| `obd_protocol` | string | e.g. `ISO15765-4 CAN11/500` or `unknown` |
| `uptime_seconds` | uint32 | Device uptime |
| `poller_status` | string | `"on"` or `"paused"` |
| `cmds_ok`, `cmds_fail`, `blocked_cmds`, `telemetry_drops` | uint64 | Runtime counters |
| `rpm_ok` … `voltage_ok` | bool | **Always sent** for all five PIDs |
| `rpm`, `speed_kmh`, etc. | varies | Only when corresponding `*_ok` is `true` (fresh ≤15 s) |
| `source` | string | Always `"esp32_obd"` |

GPS and host data are **not** in schema 1087 — they are separate events.

### Schema 1089 — GPS (`payload`)

| Field | When |
|-------|------|
| `gps_ok` | Always |
| `lat`, `lng` | Only when `gps_ok == true` |

Virtual identity: `device_id` = `gps-{suffix}`, `node_id` = `node-gps-{suffix}` where suffix is the part after the last `-` in the carrier `device_id` (e.g. `carrier-042` → `gps-042`).

### Schema 1088 — Host reading (`payload`)

One reading per event:

| Field | Type | Notes |
|-------|------|--------|
| `key` | string | e.g. `height_mm`, `signal` |
| `value` | number | Reading value |
| `unit` | string | May be empty |
| `valid` | bool | Only `valid: true` readings are uplinked |

Host `device_id` is the sensor's provisioned id (e.g. `ul212-001`). Host `node_id` is derived as `node-{host_device_id}`. `ts_ms` is the carrier's tick time (not the host's own clock), so every event in a batch shares one time base.

A host that stops reporting for more than 2 minutes drops out of the uplink entirely, so stale readings are never re-sent as if fresh.

### UL212 reading keys (`host_type_id: 1`)

| key | unit |
|-----|------|
| `height_mm` | mm |
| `smooth_mm` | mm |
| `temperature_c` | C |
| `signal` | (none) |
| `valid_echo` | (none) |
| `tilt_deg` | (none) |

---

## Wall-clock time (`ts_ms`)

When the EC200U LTE modem is up, firmware syncs **UTC epoch milliseconds**:

| Priority | Source | When | Extra AT load |
|----------|--------|------|----------------|
| 1 | `+QGPSLOC` UTC+date | GNSS fix — piggybacks the existing GPS poll | None |
| 2 | `AT+CCLK?` network time | Works indoors; every 60 s until synced, then every 30 min | 1 AT command |
| 3 | `esp_timer` uptime | Before the first sync | None |

GPS is unambiguous UTC, so once GPS has set the clock, CCLK will not move it.

India (IST): `+CCLK` reports timezone `+22` (UTC+5:30). If an operator omits the timezone field, firmware assumes IST rather than storing local time as UTC. Values outside 2025–2100 are rejected, so an unregistered modem's 1980 default is ignored.

Between syncs the clock is extrapolated from the ESP32 monotonic timer, so there is no per-tick AT traffic.

**Backend note:** `ts_ms` is UTC epoch ms only after sync. Until then it is a small uptime value (e.g. `3605000`). Treat any `ts_ms` below `1735689600000` (2025-01-01) as "device clock not yet synced" and fall back to your own receive time.

---

1. **Parse envelope** — `device_id`, `node_id`, `schemaId`, `ts_ms`, `payload` at top level.
2. **Route by `schemaId`** — `1087` OBD, `1088` host reading, `1089` GPS.
3. **Handle single object or array** on live POST.
4. **Index OBD** by `(device_id, ts_ms)`.
5. **Index GPS** by virtual `device_id` or correlate to carrier via suffix.
6. **Index host readings** by `(host device_id, ts_ms, payload.key)`.
7. **Do not conflate ids** — carrier `device_id`, virtual `gps-*`, and host `ul212-*` are separate namespaces.

---

## Differences from previous format (pre-2026-09)

| Old | New |
|-----|-----|
| One fat `1087` event with GPS + `hosts[]` nested in `payload` | Multiple typed events per tick |
| `device_id` / `node_id` inside `payload` | Identity at envelope level |
| `schemaId` only at HTTP wrapper | `schemaId` on every event |
| `schema_version` in payload | Removed — use per-event `schemaId` |
| `hosts[]` array | One `1088` event per valid reading |

---

## Related repo paths

| Path | Purpose |
|------|---------|
| `components/telemetry_uplink/uplink_payload.c` | JSON builders |
| `components/telemetry_uplink/include/uplink_schema_ids.h` | **Schema ID numbers (edit before build)** |
| `components/telemetry_uplink/uplink_schema.c` | Host type → schema lookup table |
| `hardware/fleet_telematics_carrier/host/*/host.manifest.json` | Host reading catalog |
| `docs/fleet-zigbee-host-guide.md` | Zigbee host onboarding |
| `tools/carrier_console/` | PC provisioning UI |

---

## Quick curl test

```bash
curl -sS -X POST 'https://api.trafyn.info/nc-events-api/v2/messages' \
  -H 'Content-Type: application/json' \
  -d @- <<'EOF'
{
  "device_id": "carrier-test-001",
  "node_id": "node-test-001",
  "schemaId": "1087",
  "ts_ms": 1710000000123,
  "payload": {
    "obd_profile": "fleet_basic",
    "obd_protocol": "unknown",
    "uptime_seconds": 60,
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
EOF
```
