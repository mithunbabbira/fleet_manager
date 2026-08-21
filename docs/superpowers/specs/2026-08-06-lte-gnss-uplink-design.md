# LTE GNSS lat/lng on OBD uplink — Design

Device: ESP32-C6 + Quectel EC200U (UART1 GPIO16 TX / GPIO17 RX on printed PCB)  
Date: 2026-08-06  
Status: approved  

## Goal

Attach GNSS position from the EC200U to each OBD cloud telemetry snapshot.  
OBD uplink always proceeds; GPS fields are included only when a fix exists.

## Decisions (locked)

| Topic | Choice |
|--------|--------|
| No fix | Still send OBD; `gps_ok: false`; omit `lat` / `lng` |
| With fix | `gps_ok: true` plus separate numeric `lat` and `lng` |
| How GPS is read | Background cache in `net_lte` (not blocking each uplink) |
| JSON keys | `lat`, `lng`, `gps_ok` |
| Schema | Same Trafyn schemaId `1087` payload object (coordinate with CM if keys are new) |

## Architecture

```text
EC200U GNSS  →  net_lte cache (lat, lng, gps_ok, age)
                      ↓
OBD produce tick  →  uplink_snapshot  →  SD queue / live POST
```

1. After modem AT path is healthy, enable GNSS (`AT+QGPS=1`).
2. Periodic refresh (default ~10 s): `AT+QGPSLOC` (or Quectel-recommended locate command), parse degrees.
3. Store latest fix in RAM under the modem UART mutex (shared with HTTP/OTA).
4. `telemetry_uplink` produce path copies cache into `uplink_snapshot_t` when building JSON — no extra AT call on the hot path.

## Payload shape

GPS fields live **inside** the existing `payload` object (same place as `device_id`, PIDs, etc.) — not as siblings of `schemaId`.

With fix (live single-event POST):

```json
{
  "schemaId": "1087",
  "payload": {
    "device_id": "fleet-demo-001",
    "node_id": "esp32c6-01",
    "rpm": 779.5,
    "rpm_ok": true,
    "gps_ok": true,
    "lat": 12.9716,
    "lng": 77.5946,
    "source": "esp32_obd"
  }
}
```

Without fix — still inside `payload`; omit `lat`/`lng`:

```json
{
  "schemaId": "1087",
  "payload": {
    "device_id": "fleet-demo-001",
    "node_id": "esp32c6-01",
    "gps_ok": false,
    "source": "esp32_obd"
  }
}
```

Queued / batch events keep the same inner payload object (GPS included at enqueue time when a fix was cached).

(`lat` / `lng` omitted when no fix — same policy as stale PIDs; schema rejects JSON null.)

## `net_lte` API (sketch)

```c
typedef struct {
    bool gps_ok;
    double lat;
    double lng;
    uint32_t age_ms;
} net_lte_gps_t;

esp_err_t net_lte_gps_get(net_lte_gps_t *out);
```

- Returns last cached sample; `gps_ok == false` if never fixed or stale beyond a configurable max age (e.g. 60–120 s).
- SoftAP / serial status may later expose the same cache (optional v1.1).

## Timing / contention

- GNSS AT uses the same UART mutex as QHTTP and OTA.
- Refresh interval Kconfig (default 10 s); skip refresh while `fw_ota_lte_is_busy()`.
- Uplink interval remains independent (default 5 s) and never waits on a fresh GNSS transaction.

## Hardware

- EC200U GNSS antenna required for outdoor fix.
- Indoor / no antenna → expect `gps_ok: false` indefinitely; OBD still uploads.

## Out of scope (v1)

- Altitude, SOG, COG, satellite count, NMEA streaming to host.
- Blocking uplink until first fix.
- Separate GPS-only cloud schema.
- Backend batch API work (already tracked separately).

## Test plan (device)

1. Boot with GNSS antenna outdoors → SoftAP/serial or logs show fix; uplink JSON includes `lat`/`lng`/`gps_ok: true`.
2. Indoor / antenna off → uplink continues; `gps_ok: false`; no `lat`/`lng`.
3. During LTE OTA download → GNSS refresh pauses; OBD queue/produce still works.
4. Host unit test: payload builder emits/omits GPS keys correctly.
