# Mock telemetry API + array-only uplink POST

**Date:** 2026-09-04  
**Status:** Approved  
**Branch:** `firmware-v2`

## Goal

Lab mock of Trafyn `POST …/nc-events-api/v2/messages` so the master can be validated over LTE via **ngrok**. Later, only `CONFIG_UPLINK_URL` changes to production.

## Locked body contract

- Body is **always a JSON array** of bare envelopes (never a lone object).
- **No** top-level `Vehicle` wrapper.
- Even one event: `[{ ... }]`.
- Each element requires: `device_id`, `node_id`, `schemaId`, `ts_ms`, `payload`.

## Mock server

| Item | Choice |
|------|--------|
| Path | `tools/telemetry_mock/` |
| Stack | Python stdlib `http.server` (no heavy deps) |
| Listen | `0.0.0.0:8787` |
| Route | `POST /nc-events-api/v2/messages` |
| Success | HTTP **200** + `{"ok":true,"accepted":N}` |
| Reject | **400** if not array / missing fields (clear error JSON) |
| Log | stdout + append `tools/telemetry_mock/received.jsonl` |

Also: `GET /health` → `{"ok":true}`.

## ngrok / URL swap

1. Run mock locally.  
2. `ngrok http 8787`.  
3. Set master `CONFIG_UPLINK_URL` to  
   `https://<ngrok-host>/nc-events-api/v2/messages`.  
4. Production: same path on Trafyn host — **URL only**.

## Firmware (`firmware_v2` uplink)

- Remove `{"Vehicle":…}` wrap on live POST and SD drain.
- Always POST a JSON **array** of bare envelopes (single event → length-1 array).
- SD queue still stores bare envelopes; drain builds `[e1,e2,…]`.

## Non-goals

- Auth headers, schema-registry emulation beyond field checks.
- Changing Zigbee/OBD producers beyond POST shape.
- Editing legacy `components/telemetry_uplink` (reference only).

## Success

1. `curl` array body → mock 200 + jsonl line.  
2. Master with ngrok URL POSTs 1089/1087/1088 as arrays → mock accepts.  
3. Switching URL to Trafyn needs no other path/shape change (assuming Trafyn matches this contract).
