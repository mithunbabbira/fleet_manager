# Carrier Console — Diagnostics v2: GPS / uplink / SD queue (design)

**Date:** 2026-09-03  
**Status:** Approved (parse-only)  
**Depends on:** [Diagnostics panel v1](./2026-09-03-carrier-console-diagnostics-design.md)  
**Surface:** `tools/carrier_console/`

## Goal

Extend the Diagnostics card so operators can see **GPS fix**, **last uplink HTTP result**, and **SD queue depth** without grepping the serial log — still with **no firmware changes**.

## Non-goals

- No new CLI commands or status-line format changes on the carrier.
- No Host Console / BLE changes.
- No automatic cloud POST from the UI beyond existing **Send now** / **Queue test**.

## Data sources (existing serial)

From bare `uplink` (status dump) in `transport_serial.c`:

```text
uplink: enabled=… interval=… device_id=… node_id=…
        last: ok=… skipped=… http=N reason="…" error="…"
        queue: sd=yes|no depth=N bytes=N drain_err="…"
        url=… schemaId=…
        gps: ok lat=… lng=… age_ms=N
        # or
        gps: no fix
```

Also opportunistic:

- `uplink now: … http=N reason="…" error="…"`
- `uplink qtest: … sd=… depth=N …`

## UI

Under **Carrier Zigbee** in Diagnostics, add **Carrier path**:

| Tile | Display |
|------|---------|
| GPS | `ok` + lat/lng + age, or `no fix` / `—` |
| Last uplink | `http=N · ok/skip · reason` (error if present) |
| SD queue | `mounted · depth=N · bytes=N` (drain_err if non-empty) |

**Refresh hosts** (and Diagnostics refresh) also issues `uplink` after `status` / `fleet hosts` so these lines arrive without a separate button.

**Copy fleet snapshot** JSON gains `carrier_path` with `gps`, `uplink_last`, and `queue` objects (nulls when unknown).

## Parsing

Frontend `parseRxLogOnly` regexes (same pattern as existing uplink fail/ok). Backend `refresh_dashboard` appends `uplink` so the poll path fills tiles.

## Success criteria

1. After connect + refresh, Diagnostics shows GPS / last / queue when the carrier prints those lines.
2. Copy snapshot includes `carrier_path`.
3. No firmware diff required for the happy path.

## Testing (manual)

1. GPS ok / no fix tiles update from `gps:` lines.
2. `last:` / `uplink now:` update HTTP tile.
3. `queue:` updates SD tile.
4. Disconnect → tiles show `—`.
