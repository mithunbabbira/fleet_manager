# Telemetry mock API (lab)

Replicates `POST /nc-events-api/v2/messages` for master uplink tests. Body contract matches production intent:

- **Always a JSON array** of bare envelopes (even one event).
- No `Vehicle` wrapper.

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
  -d '[
    {
      "device_id": "fleet-demo-001_GPS",
      "node_id": "node-fleet-demo-001_GPS",
      "schemaId": "1089",
      "ts_ms": 1710000001000,
      "payload": { "gps_ok": true, "lat": 12.97, "lng": 77.59 }
    }
  ]'
```

Expect `{"ok":true,"accepted":1}`. Events append to `received.jsonl`.

## ngrok → master

```bash
ngrok http 8787
```

Set the master URL to the **full** path (rebuild or sdkconfig):

```
CONFIG_UPLINK_URL="https://YOUR-SUBDOMAIN.ngrok-free.app/nc-events-api/v2/messages"
```

On device: provision `device_id` / `node_id`, then `uplink once`.

## Swap to production later

Change only `CONFIG_UPLINK_URL` to Trafyn:

```
https://api.trafyn.info/nc-events-api/v2/messages
```

Path and body shape stay the same (assuming Trafyn accepts array-of-envelopes).

## Spec

`docs/superpowers/specs/2026-09-04-telemetry-mock-array-uplink-design.md`
