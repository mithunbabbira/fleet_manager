# Session review — 2026-09-02

Branch: `review/2026-09-02-uplink-stability-lte-fix`  
Base: `feat/trafyn-firmware-ota` @ `8f1137c`

## Cloud JSON schema (manager recommendation) — **kept**

Each uplink tick emits **0..N typed events**. Envelope fields are **top-level**:

```json
{
  "device_id": "...",
  "node_id": "...",
  "schemaId": "1087|1088|1089",
  "ts_ms": 1710000001000,
  "payload": { ... }
}
```

Live POST: single object when count==1, **JSON array** when count>1.  
Batch POST (SD drain): always a JSON array.

| schemaId | Type | `device_id` |
|----------|------|-------------|
| `1087` | OBD / vehicle | provisioned carrier id |
| `1088` | One host reading | host id (e.g. `ul212-001`) |
| `1089` | GPS | virtual `gps-{suffix}` |

`ts_ms`: wall-clock UTC epoch ms from modem CCLK/GPS when available; else uptime ms.

See `docs/telemetry-api-backend-guide.md` and `uplink_schema_ids.h`.

## Stability fixes (also kept)

| Area | Why |
|------|-----|
| ESP32-C6 UART1 clock enable (`esp_private/uart_private.h` on IDF 5.2) | Fixes interrupt-WDT boot loop after Zigbee |
| GPS task starts only after first AT OK | Avoids UART contention |
| AT attempts 40 → 60 | Cold EC200U boot |
| Wall-clock via CCLK / GPS | Real `ts_ms` |
| SD queue hardening + produce/drain mutex | Reliability |
| Host registry LRU, non-fatal Zigbee/CAN | Bench resilience |

## Note on trim attempt

An intermediate commit briefly reverted the multi-event schema back to the old monolithic `{schemaId,payload}` shape. That was incorrect relative to the manager’s schema change and has been **restored**.
