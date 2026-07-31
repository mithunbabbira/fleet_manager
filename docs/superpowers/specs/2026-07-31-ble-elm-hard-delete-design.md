# BLE/ELM327 Hard Delete (size trim for OTA headroom)

Branch: `feature/mcp2515-can` · Date: 2026-07-31  
Repo: `63idealabs/fleet-telematics-node`

## Goal

Remove the unused BLE ELM327 Mini path from the fleet telematics node so the
firmware no longer links NimBLE/Bluetooth. SoftAP web UI and LTE uplink stay.
This is a size/complexity optimize pass before A/B OTA over LTE.

## Non-goals

- SoftAP / Wi-Fi removal
- OTA download/flash implementation (follow-up)
- J1939 / new PIDs
- Changing Trafyn schema IDs

## Current state

- Live OBD path: `can_obd` (MCP2515) → `obd_poller` → `telemetry_bus` → SoftAP + uplink
- Still compiled and initialized (idle): `ble_elm`, `elm327_client`, `elm_transport`
- Serial/HTTP still expose BLE scan/connect and call `elm327_client_*` in places
- Flash: ~1.27 MiB; BLE stack ~200–280 KiB of that

## Decision

**Hard delete** the BLE/ELM path from the build and product APIs (Approach 1).
Do not keep a Kconfig feature flag. History remains in git if ever needed.

## Design

### Components to stop linking (then remove from tree)

| Component | Action |
|-----------|--------|
| `components/ble_elm` | Delete |
| `components/elm327_client` | Delete |
| `components/elm_transport` | Delete (only used by ELM/BLE) |

### Bluetooth / sdkconfig

In `sdkconfig.defaults` (and regenerate/clean `sdkconfig` as needed):

- Disable Bluetooth / NimBLE (`CONFIG_BT_ENABLED` unset / NimBLE off)
- Drop any ELM BLE scan/timeout Kconfig that only served BLE (or leave unused if shared)

### `main/app_main.c`

- Remove `ble_elm_init` / `elm327_client_init`
- Boot path: NVS → runtime → profiles → telemetry → LTE → uplink → **can_obd** → serial → HTTP → poller → can_boot
- CMake: drop `ble_elm`, `elm327_client` from `REQUIRES`

### `transport_serial`

Remove commands: `scan`, `devices`, `select`, `unbond`, and BLE-oriented `init`.

Keep: `help`, `status`, `cmd`, `profiles`, `profile`, `telemetry`, `unsafe`, `metrics`, `lte`, `uplink`.

- `status`: report `can_ready`, active protocol string, poller, profile, metrics (no `ble_connected` / `elm_ready`)
- `cmd <hex>`: policy check + `can_obd_transact` (same safety gate as poller)

### `transport_http` / SoftAP UI

- Remove BLE scan / connect / disconnect / bond API handlers
- Status endpoints: use `can_obd_is_ready()` + `can_obd_get_protocol()`
- Raw OBD / profile / uplink: readiness gated on CAN link
- Update embedded UI (`static_index.html.h`) to drop BLE device picker; show CAN link/protocol instead
- CMake: drop `ble_elm`, `elm327_client` from `REQUIRES`

### Profiles / uplink

- Profiles remain; AT init sequences ignored (no ELM)
- Uplink already CAN-gated; keep `adapter_name: MCP2515` and live protocol string
- Schema fields `ble_connected` / `elm_ready` may continue to mirror CAN link for API compat (document as such) or be left as-is from prior CAN work

### Docs

- Update `PROJECT_CONTEXT.md` / README notes: product is MCP2515 + SoftAP + LTE, not BLE ELM327
- Point to this spec from the OTA follow-up

## Success criteria

1. Clean `idf.py build` with Bluetooth disabled
2. `idf.py size` no longer lists `libble_app` / NimBLE archives; flash image **noticeably smaller** (expect ~150–280 KiB)
3. In-car / bench: poller streams PIDs; SoftAP UI works without BLE screens; LTE uplink still POSTs
4. Serial `cmd 010C` returns a Mode 01 response when CAN link is up
5. Host unit tests still pass (`cmd_policy`, `obd_codec`, `obd_isotp`, `uplink_payload`)

## Risks

| Risk | Mitigation |
|------|------------|
| HTTP/UI still references BLE symbols | Grep + compile will catch; update UI in same change |
| Someone needs ELM later | Restore from git on a branch; not supported on truck product |
| sdkconfig local overrides re-enable BT | Document clean build / defaults |

## Follow-up (not this change)

1. A/B partition table (`ota_0` + `ota_1`)
2. LTE HTTPS firmware download + verify + rollback
3. Optional SoftAP trim if more flash headroom needed
