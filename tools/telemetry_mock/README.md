# Telemetry mock API (lab)

Replicates `POST /nc-events-api/v2/messages` for master uplink tests.

Accepted bodies (same as current firmware with `CONFIG_UPLINK_VEHICLE_WRAP=y`):

- `{"Vehicle":[ {envelope}, ... ]}` — Trafyn-compatible wrap (preferred for device tests)
- bare `[ {envelope}, ... ]` — also accepted

Each envelope needs: `device_id`, `node_id`, `schemaId`, `ts_ms`, `payload`.

## Run

```bash
cd tools/telemetry_mock
python3 server.py
```

Listens on `0.0.0.0:8787`.

Health: `curl -s http://127.0.0.1:8787/health`

## Curl smoke test

```bash
curl -sS -X POST 'http://127.0.0.1:8787/nc-events-api/v2/messages' \
  -H 'Content-Type: application/json' \
  -d '{"Vehicle":[{"device_id":"fleet-demo-001_GPS","node_id":"node-fleet-demo-001_GPS","schemaId":"1089","ts_ms":1710000001000,"payload":{"gps_ok":true,"lat":12.97,"lng":77.59}}]}'
```

Expect `{"ok":true,"accepted":1}`. Events append to `received.jsonl`.

## ngrok → master

```bash
ngrok http 8787
```

On the device (no rebuild required for identity):

```text
device_id <id>
node_id <id>
```

Telemetry POST URL is **from the firmware bin** — edit only  
`firmware_v2/master/sdkconfig.defaults` (`CONFIG_UPLINK_URL` / `CONFIG_OTA_CLOUD_CHECK_URL`), then rebuild/OTA.  
`uplink url` on the device is read-only.

```text
uplink url
uplink once
```

When finished labbing, stop ngrok/mock and restore:

```text
uplink url https://api.trafyn.info/nc-events-api/v2/messages
```

## Swap to production later

Same CLI URL change. Path and envelope fields stay the same; Trafyn must accept the Vehicle-wrapped array (backend schema-registry must be healthy).

## Spec

`docs/superpowers/specs/2026-09-04-telemetry-mock-array-uplink-design.md`
