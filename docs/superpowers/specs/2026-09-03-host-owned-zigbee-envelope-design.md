# Host-owned Zigbee uplink envelope (design)

**Date:** 2026-09-03  
**Status:** Approved (approach A) — implementing  
**Branch:** `review/2026-09-02-uplink-stability-lte-fix`

## Goal

New Zigbee hosts join without carrier C changes for schema mapping. Cloud envelope fields come from the host:

- `device_id`, `node_id`, `schemaId`, `payload` (keys/units)

## Wire protocol

Header TLVs (in addition to existing):

| ID | Name | Type | Required for dynamic uplink |
|----|------|------|-----------------------------|
| 8 | `NODE_ID` | string | yes |
| 9 | `SCHEMA_ID` | string | yes |
| 10 | `HOST_TYPE` | string | optional (display / payload `host_type`) |

HELLO body: zero or more `METRIC_DEF` (ID 11, string) entries:

`{tlv_id}:{key}:{unit}:{type}`  
Examples: `16:height_mm:mm:f`, `19:signal::u8`  
Types: `f`, `u8`, `u16`, `i32`, `s`

REPORT body: compact readings by `tlv_id` (unchanged value encoding). REPORT header repeats `NODE_ID` + `SCHEMA_ID`.

## Carrier behavior

1. Accept frames with `device_id` + (`schema_id`+`node_id` **or** known manifest `host_type_id`).
2. Apply `METRIC_DEF` into per-host reading slots (dynamic; no catalog required).
3. Legacy: if no schema on wire but catalog has host → keep old key mapping (REPORT still works); uplink still needs `schema_id` from wire (host firmware update required for cloud).
4. Uplink: use host `device_id` / `node_id` / `schema_id`; build payload from registry keys. Remove `uplink_schema.c` UL212 hardcode.

## Host UL212

- Defaults: `device_id=ul212-001`, `node_id=node-ul212-001`, `schema_id=1088`, `host_type=ul212_ble_fetch`
- HELLO sends metric defs from manifest readings
- Config NVS can override ids

## Out of scope

- OBD 1087 / GPS 1089 stay carrier-defined
- Full JSON payload blob over Zigbee
