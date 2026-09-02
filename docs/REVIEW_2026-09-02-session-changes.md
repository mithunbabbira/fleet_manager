# Session review — 2026-09-02 (trimmed)

Branch: `review/2026-09-02-uplink-stability-lte-fix`  
Base: `feat/trafyn-firmware-ota` @ `8f1137c`

## What this branch keeps (sensible)

| Area | Why |
|------|-----|
| ESP32-C6 UART1 clock enable before `uart_driver_install` | Fixes interrupt-WDT boot loop after Zigbee |
| GPS task starts only after first AT OK | Avoids UART contention during modem bring-up |
| AT attempts 40 → 60 | Cold EC200U can take 30–60 s |
| Soft fail on `uart_param_config` / `uart_set_pin` | Avoids abort on pin errors |
| Skip `net_lte_refresh` while OTA owns UART | Prevents AT pile-up during OTA |
| Wall-clock via CCLK / GPS (`net_lte_time_now_ms`) | Better `ts_ms` when modem has time; uptime fallback |
| `store_sd` I/O fail → unmount, safer compact, meta reconcile | Queue does not stall forever |
| `s_io_mu` around produce/drain | Shared static buffers + modem serialization |
| Drop unusable single SD record | Drain never stuck on corrupt line |
| Host registry LRU when full | New hosts can still register |
| Larger host key / type buffers | Match manifest limits |
| Zigbee / CAN start non-fatal in `app_main` | Bench without radio/bus still boots |

## What was removed (overcomplicated / broke things)

The earlier rewrite changed cloud JSON to **multi-event** envelopes:

```json
{ "device_id", "node_id", "schemaId", "ts_ms", "payload": { ... } }
```

with separate schema IDs (1087 / 1088 / 1089), one event per host reading, virtual GPS device IDs, and a large rewrite of `uplink_payload.*` + `telemetry_uplink.c`.

That is **reverted**. Uplink is back to the known-good Trafyn shape:

```json
{ "schemaId": "1087", "payload": { ... } }
```

Batch POST remains a JSON array of those objects. See `docs/telemetry-api-backend-guide.md`.

Removed files: `uplink_schema.c`, `uplink_schema.h`, `uplink_schema_ids.h`.

## LTE UART pins (unchanged)

`CONFIG_NET_LTE_UART_TX_GPIO=16`, `CONFIG_NET_LTE_UART_RX_GPIO=17` in `sdkconfig.defaults`.

## Verify

```bash
cmake -S tests/host -B build-host && cmake --build build-host && ctest --test-dir build-host
idf.py build
```
