# Review branch notes — 2026-09-02 uplink stability + multi-event schema

Branch: `review/2026-09-02-uplink-stability-lte-fix`  
Known-good baseline for Zigbee/hosts: `feat/trafyn-firmware-ota` @ `8f1137c`

## Intent (do not undo)

Manager-requested **multi-event** cloud schema is the target design:

- Top-level envelope: `device_id`, `node_id`, `schemaId`, `ts_ms`, `payload`
- `1087` OBD · `1088` one host reading · `1089` GPS
- Wall-clock `ts_ms` when modem time is available
- Cleaner decoupling: OBD / GPS / host are separate events

Stability work that belongs with this branch:

- ESP32-C6 UART1 clock enable (IDF 5.2: `esp_private/uart_private.h`)
- GPS AT deferred until modem AT OK
- SD queue hardening, produce/drain mutex
- Non-fatal Zigbee/CAN boot

## What “make it work” means here

Compare against the older branch for **runtime regressions** (boot loop, UART hang, produce/drain stalls), not to roll back the schema. Fix build/flash/runtime on this machine; keep the new structure.

## Fixes applied on this machine (keep schema)

| Issue | Fix |
|-------|-----|
| Host `1088` `value` was garbage (`3e-314`) | Pass `reading->value` into `appendf(..., "%.4g", …)` in `uplink_payload_build_host_reading_payload` |
| GPS move/heartbeat vs wall-clock jump | Dedup uses monotonic `now_ms()`; event `ts_ms` still prefers LTE wall clock |
| Tight per-event buffer | `EVENT_BUF_LEN` 1024 → 1536 |
| Unit test gap | Assert `"value":40.9` in `test_uplink_payload` |

## Verify

```bash
cmake -S tests/host -B build-host && cmake --build build-host && ctest --test-dir build-host
idf.py build && idf.py -p /dev/cu.usbmodemXXXX flash
```
