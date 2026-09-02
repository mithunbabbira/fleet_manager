# Fleet telemetry API — backend integration guide

**Audience:** Backend / API developers consuming carrier uplink events  
**Firmware source of truth:** `components/telemetry_uplink/uplink_payload.c`  
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

The POST URL and schema id can be overridden per device in NVS (Carrier Console / serial). Factory defaults are defined in `components/telemetry_uplink/include/uplink_payload.h` (`UPLINK_URL`, `UPLINK_SCHEMA_ID`).

---

## Request body shapes

The device sends **one of two** JSON shapes to the same URL:

### Single event (live POST, no SD card)

```json
{
  "schemaId": "1087",
  "payload": { ... }
}
```

### Batch (SD queue drain)

```json
[
  { "schemaId": "1087", "payload": { ... } },
  { "schemaId": "1087", "payload": { ... } }
]
```

**Parsing rule:** if the root JSON value is an array, treat each element as an event; otherwise treat the root object as a single event.

```javascript
const body = await request.json();
const events = Array.isArray(body) ? body : [body];
```

Return **HTTP 2xx** only on successful ingest. The device keeps queued events on SD until drain gets 2xx (non-2xx leaves the queue head in place).

---

## Identity (carrier vs hosts)

| Field | Scope | Description |
|-------|--------|-------------|
| `payload.device_id` | **Carrier** | Unique telematics unit ID — provisioned once per device (NVS). Primary fleet key. |
| `payload.node_id` | **Carrier** | Secondary node id — provisioned with `device_id`. |
| `hosts[].device_id` | **Sensor host** | ID of an attached Zigbee sensor (e.g. `ul212-001`). **Not** the carrier id. |

Fresh units ship **unprovisioned**; uplink is blocked until `device_id` + `node_id` are set via Carrier Console or serial `provision <device_id> <node_id>`.

---

## Sample 1 — Full event (vehicle + GPS + OBD + two hosts)

Primary integration fixture:

```json
{
  "schemaId": "1087",
  "payload": {
    "device_id": "carrier-042",
    "node_id": "node-042",
    "schema_version": 1,
    "ts_ms": 1710000000123,
    "obd_profile": "fleet_basic",
    "obd_protocol": "ISO15765-4 CAN11/500",
    "uptime_seconds": 3600,
    "poller_status": "on",
    "cmds_ok": 1432,
    "cmds_fail": 3,
    "blocked_cmds": 0,
    "telemetry_drops": 0,

    "rpm": 780.5,
    "rpm_raw_hex": "410C0C30",
    "rpm_age_ms": 120,
    "rpm_ok": true,

    "speed_kmh": 42.0,
    "speed_raw_hex": "410D2A",
    "speed_age_ms": 120,
    "speed_ok": true,

    "coolant_c": 87.0,
    "coolant_raw_hex": "41057F",
    "coolant_age_ms": 120,
    "coolant_ok": true,

    "throttle_pct": 14.5,
    "throttle_raw_hex": "411125",
    "throttle_age_ms": 120,
    "throttle_ok": true,

    "voltage_v": 13.7,
    "voltage_raw": "13.7V",
    "voltage_age_ms": 120,
    "voltage_ok": true,

    "gps_ok": true,
    "lat": 12.9715987,
    "lng": 77.5945627,

    "hosts": [
      {
        "device_id": "ul212-001",
        "host_type": "ul212_ble_fetch",
        "host_type_id": 1,
        "readings": [
          { "key": "height_mm", "value": 40.5, "unit": "mm", "valid": true },
          { "key": "smooth_mm", "value": 40.2, "unit": "mm", "valid": true },
          { "key": "signal", "value": 72, "unit": "", "valid": true }
        ],
        "ts_ms": 1710000000100
      },
      {
        "device_id": "temp-host-002",
        "host_type": "future_sensor",
        "host_type_id": 2,
        "readings": [
          { "key": "temperature_c", "value": 28.3, "unit": "C", "valid": true }
        ],
        "ts_ms": 1710000000110
      }
    ],

    "source": "esp32_obd"
  }
}
```

---

## Sample 2 — Bench / no vehicle (no OBD, no hosts)

Typical indoors: no CAN ECU, no Zigbee hosts:

```json
{
  "schemaId": "1087",
  "payload": {
    "device_id": "carrier-bench-01",
    "node_id": "node-bench-01",
    "schema_version": 1,
    "ts_ms": 1710000005000,
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

    "gps_ok": false,

    "source": "esp32_obd"
  }
}
```

---

## Sample 3 — Batch POST (array of two events)

```json
[
  {
    "schemaId": "1087",
    "payload": {
      "device_id": "carrier-042",
      "node_id": "node-042",
      "schema_version": 1,
      "ts_ms": 1710000001000,
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
      "gps_ok": true,
      "lat": 12.9716000,
      "lng": 77.5945600,
      "source": "esp32_obd"
    }
  },
  {
    "schemaId": "1087",
    "payload": {
      "device_id": "carrier-042",
      "node_id": "node-042",
      "schema_version": 1,
      "ts_ms": 1710000006000,
      "obd_profile": "fleet_basic",
      "obd_protocol": "ISO15765-4 CAN11/500",
      "uptime_seconds": 3610,
      "poller_status": "on",
      "cmds_ok": 1445,
      "cmds_fail": 3,
      "blocked_cmds": 0,
      "telemetry_drops": 0,
      "rpm_ok": false,
      "speed_kmh": 0.0,
      "speed_raw_hex": "410D00",
      "speed_age_ms": 90,
      "speed_ok": true,
      "coolant_ok": false,
      "throttle_ok": false,
      "voltage_ok": false,
      "gps_ok": true,
      "lat": 12.9716010,
      "lng": 77.5945610,
      "hosts": [
        {
          "device_id": "ul212-001",
          "host_type": "ul212_ble_fetch",
          "host_type_id": 1,
          "readings": [
            { "key": "height_mm", "value": 40.5, "unit": "mm", "valid": true }
          ],
          "ts_ms": 1710000005980
        }
      ],
      "source": "esp32_obd"
    }
  }
]
```

---

## Payload field reference (carrier)

### Always present

| Field | Type | Notes |
|-------|------|--------|
| `device_id` | string | Carrier identity |
| `node_id` | string | Carrier node identity |
| `schema_version` | int | Currently `1` |
| `ts_ms` | uint64 | Event capture time (ms, device monotonic clock) |
| `obd_profile` | string | Active OBD poll profile name |
| `obd_protocol` | string | e.g. `ISO15765-4 CAN11/500` or `unknown` |
| `uptime_seconds` | uint32 | Device uptime |
| `poller_status` | string | `"on"` or `"paused"` |
| `cmds_ok` | uint64 | Runtime counter |
| `cmds_fail` | uint64 | Runtime counter |
| `blocked_cmds` | uint64 | Commands blocked by safety policy |
| `telemetry_drops` | uint64 | Bus subscription drops |
| `rpm_ok` … `voltage_ok` | bool | **Always sent** for all five PIDs |
| `gps_ok` | bool | GNSS fix available |
| `source` | string | Always `"esp32_obd"` |

### OBD PID fields (conditional)

Fresh window: **15 seconds** (`UPLINK_PID_FRESH_MS`).

| When `*_ok` is `true` | Also present |
|------------------------|--------------|
| `rpm_ok` | `rpm`, `rpm_raw_hex`, `rpm_age_ms` |
| `speed_ok` | `speed_kmh`, `speed_raw_hex`, `speed_age_ms` |
| `coolant_ok` | `coolant_c`, `coolant_raw_hex`, `coolant_age_ms` |
| `throttle_ok` | `throttle_pct`, `throttle_raw_hex`, `throttle_age_ms` |
| `voltage_ok` | `voltage_v`, `voltage_raw`, `voltage_age_ms` |

**Important:** When `*_ok` is `false`, the numeric/raw/age fields are **omitted** (not sent as `null`). Backend must not assume `0` for missing fields.

A parked vehicle may legitimately send `speed_kmh: 0` with `speed_ok: true`.

### GPS fields (conditional)

| Field | When |
|-------|------|
| `lat`, `lng` | Only when `gps_ok == true` |

The device may uplink GPS-only snapshots when CAN/OBD is unavailable.

### `hosts` (optional)

| Field | When |
|-------|------|
| `hosts` | Only when ≥1 Zigbee host has reported (max **4** hosts per event) |

If no hosts are joined, the `hosts` key is **absent** (not an empty array).

---

## Handling `hosts[]` (dynamic sensor list)

Each carrier event may include zero or more **attached sensor hosts** (Zigbee end devices). This is an **extensible** list — different host types expose different reading keys.

### Host object shape

```json
{
  "device_id": "ul212-001",
  "host_type": "ul212_ble_fetch",
  "host_type_id": 1,
  "readings": [
    { "key": "height_mm", "value": 40.5, "unit": "mm", "valid": true }
  ],
  "ts_ms": 1710000000100
}
```

| Field | Description |
|-------|-------------|
| `device_id` | Host sensor id (separate namespace from carrier `payload.device_id`) |
| `host_type` | String label from host manifest (e.g. `ul212_ble_fetch`) |
| `host_type_id` | Numeric type — **use for server-side schema lookup** |
| `readings` | 0–8 items; keys/units vary by host type |
| `readings[].key` | Metric name (manifest-defined) |
| `readings[].value` | Numeric value |
| `readings[].unit` | Unit string (may be empty) |
| `readings[].valid` | If `false`, skip this reading |
| `ts_ms` | Host report timestamp (ms) |

### Reference: UL212 host (`host_type_id: 1`)

From `hardware/fleet_telematics_carrier/host/ul212-ble-fetch/host.manifest.json`:

| key | unit | Description |
|-----|------|-------------|
| `height_mm` | mm | Liquid height (primary) |
| `smooth_mm` | mm | Smoothed height |
| `temperature_c` | C | Temperature |
| `signal` | (none) | BLE signal strength |
| `valid_echo` | (none) | Echo validity flag |
| `tilt_deg` | (none) | Tilt |

Future host types will add new `host_type_id` values and new `readings[].key` sets.

### Backend implementation checklist

1. **Accept variable `hosts` length** — 0 to 4; do not require the field.
2. **Route by `schemaId`** (`"1087"`) then parse `payload`.
3. **Index carrier events** by `payload.device_id` + `payload.ts_ms`.
4. **Index host metrics** by `(carrier device_id, hosts[].device_id, hosts[].ts_ms, readings[].key)`.
5. **Maintain a host type catalog** keyed by `host_type_id` (mirror device manifests under `hardware/fleet_telematics_carrier/host/`).
6. **Parse readings generically** — loop keys; do not hard-code only UL212 fields on the carrier schema.
7. **Skip invalid readings** — ignore entries where `valid == false`.
8. **Tolerate unknown keys** — store raw JSON or drop unknown keys; do not fail the whole request when a new host type appears before the catalog is updated.
9. **Do not conflate ids** — `payload.device_id` is the truck box; `hosts[].device_id` is the sensor.

### Pseudocode

```python
def ingest_event(event: dict) -> None:
    if event.get("schemaId") != "1087":
        raise BadRequest("unsupported schemaId")

    p = event["payload"]
    if not p.get("device_id") or not p.get("node_id"):
        raise BadRequest("device_id and node_id required")

    store_carrier_snapshot(p)

    for host in p.get("hosts") or []:
        host_id = host["device_id"]
        type_id = host["host_type_id"]
        for r in host.get("readings") or []:
            if not r.get("valid", True):
                continue
            store_host_reading(
                carrier_id=p["device_id"],
                host_id=host_id,
                host_type_id=type_id,
                key=r["key"],
                value=r["value"],
                unit=r.get("unit", ""),
                ts_ms=host["ts_ms"],
            )
```

---

## Validation & error responses

Suggested **400 Bad Request** when:

- Body is not valid JSON
- Root is neither object nor array of objects
- `schemaId` missing or not registered
- `payload.device_id` or `payload.node_id` missing or empty

Suggested **2xx** when the event(s) are accepted (even if some optional fields are unknown — prefer ingest + quarantine over hard reject).

---

## Differences from older documentation

| Old (BLE / SoftAP era) | Current (MCP2515 carrier) |
|------------------------|---------------------------|
| `ble_connected`, `elm_ready`, `ble_peer_address` | **Removed** — CAN path only |
| Stale PIDs as JSON `null` | Stale PIDs **omitted**; `*_ok: false` always present |
| Fixed payload only | Optional **`hosts[]`** array for Zigbee sensors |
| Shared demo `device_id` | **Per-device provisioned** ids (no factory default) |

---

## Related repo paths

| Path | Purpose |
|------|---------|
| `components/telemetry_uplink/uplink_payload.c` | JSON builder (authoritative field list) |
| `components/telemetry_uplink/include/uplink_payload.h` | URL, schema id, struct limits |
| `hardware/fleet_telematics_carrier/host/*/host.manifest.json` | Host reading key catalog |
| `docs/fleet-zigbee-host-guide.md` | Zigbee host onboarding |
| `tools/carrier_console/` | PC provisioning UI |

---

## Quick curl test (backend local)

Replace URL and body as needed:

```bash
curl -sS -X POST 'https://api.trafyn.info/nc-events-api/v2/messages' \
  -H 'Content-Type: application/json' \
  -d @- <<'EOF'
{
  "schemaId": "1087",
  "payload": {
    "device_id": "carrier-test-001",
    "node_id": "node-test-001",
    "schema_version": 1,
    "ts_ms": 1710000000123,
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
    "gps_ok": false,
    "source": "esp32_obd"
  }
}
EOF
```
