# LTE Firmware OTA — Design (Phase 2)

Date: 2026-08-03  
Depends on: SoftAP `fw_ota` (Phase 1, proven)  
Lab host: `tools/ota_dev_server` + ngrok

## Goal

Pull manifest + `.bin` over EC200U QHTTP, stream into existing `fw_ota` writer, reboot, confirm.

## Modules

| Module | Role |
|--------|------|
| `net_lte` | Add `http_get` (small body) + `http_get_stream` (chunk callback) |
| `fw_ota` | Unchanged flash/hash/reboot/confirm |
| `fw_ota_lte` | NVS config, manifest parse, version check, orchestrate download → `fw_ota_*` |
| SoftAP `/api/ota/lte` | Configure URL/channel + trigger check |

## Flow

1. Ensure PDP/IP up (existing).
2. `GET {manifest_url}?device_id=&channel=` → JSON.
3. If `version` equals running and not `force` → stop (`no_update`).
4. `fw_ota_begin(size, sha256)`.
5. Stream `GET {url}` body chunks → `fw_ota_write`.
6. `fw_ota_end_and_reboot()`.
7. Boot: existing SoftAP health gate confirms image.

## NVS (`elm` namespace)

| Key | Meaning |
|-----|---------|
| `ota_manif` | Full manifest URL (ngrok/CM) |
| `ota_chan` | `stable` (default) |
| `ota_force` | `0/1` force even if version matches |

`device_id` reused from uplink NVS / defaults.

## SoftAP

- `GET /api/ota/lte` — config + last result  
- `POST /api/ota/lte` — save URL/channel/force  
- `POST /api/ota/lte/run` — start background check (returns immediately; poll `/api/ota`)

## Out of scope

Signed manifests. SoftAP multipart (already done). Auto-check: see `2026-08-03-lte-ota-auto-check-design.md`.
