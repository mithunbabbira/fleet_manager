# Trafyn firmware check OTA (replace lab GET manifest)

**Date:** 2026-08-19  
**Status:** approved for planning  
**Depends on:** `fw_ota` flash/rollback, `net_lte` QHTTP GET stream, auto-check timing (`2026-08-03-lte-ota-auto-check-design.md`)  
**Replaces:** lab GET `/firmware/manifest` (ngrok) and SoftAP `.bin` upload as update sources

## Goal

The device learns firmware updates from Trafyn’s **POST** `get-latest-device-firmware` over EC200U LTE, then streams the S3 **presigned URL** into the existing dual-bank `fw_ota` writer. QA and prod share the same request/response shape; only host and optional auth headers change in config.

## Non-goals

- Changing Trafyn POST body keys or the response envelope later.
- SoftAP `.bin` upload (lab-only; remove).
- Lab GET manifest / ngrok (`tools/ota_dev_server` client path; remove from firmware and Kconfig).
- PPP, new TLS stack, or signed manifests beyond server-provided SHA-256.

## Locked decisions

| Topic | Decision |
|---|---|
| Integration style | Translator: POST Trafyn → map `data` to internal `{version, url, sha256, size}` → existing GET-stream + `fw_ota_*` |
| `deviceId` | NVS/SoftAP `device_id` (default `fleet-demo-001`) |
| `manufacturer` | `Espressif Systems` (Kconfig) |
| `deviceType` | `fleet monitor` (Kconfig) |
| `currentVersion` | Numeric/semver prefix of running app version: `1.0.4-lab` → `1.0.4` |
| Integrity | Trafyn `data.sha256` (lowercase hex) + `data.size`; refuse flash if either missing or mismatch |
| Auth now | None. Kconfig strings for `Authorization` and `x-nc-system-user-id` default empty (headers omitted) |
| Auth later | Fill those Kconfig values; do not change JSON body/response |
| Local update | Remove SoftAP `POST /api/ota` upload and GET-manifest client |
| SoftAP | Keep UI for stats (`device_id` editable; OTA phase/versions/error; no ngrok URL field) |
| Auto-check | Unchanged: wait ~90 s for LTE, then every 24 h; auto always `force=false` |

## Request

`POST {firmware_check_url}`

QA default URL (Kconfig):

```
https://api.trafyn.info/workflow-engine/realm/1/user/1/v1/execution/service/runWithNoAuth/get-latest-device-firmware?refreshCache=true
```

Prod: same path, different host, same query. Full URL is one config string.

Headers (only if the corresponding Kconfig value is non-empty):

- `Content-Type: application/json` (always)
- `Authorization: Bearer <token>`
- `x-nc-system-user-id: <id>`

Body (frozen):

```json
{
  "input": {
    "deviceId": "fleet-demo-001",
    "manufacturer": "Espressif Systems",
    "deviceType": "fleet monitor",
    "currentVersion": "1.0.4"
  }
}
```

Version strip rule: take `esp_app_desc->version`; if a `-` exists, use the substring before the first `-`. If the result is empty, send the full version string.

## Response (frozen envelope + two new `data` fields)

```json
{
  "success": true,
  "status": 200,
  "errorCode": null,
  "errorMessage": null,
  "data": {
    "environment": "staging",
    "latestVersion": "1.0.7",
    "presignedUrl": "https://…amazonaws.com/….bin?X-Amz-…",
    "updateAvailable": true,
    "deviceId": "fleet-demo-001",
    "sha256": "e39a3fe9…64 hex chars…",
    "size": 1109248
  }
}
```

Trafyn QA must add `sha256` and `size` on `data` before device implementation lands. Device does not invent them.

## Device flow

1. Ensure LTE PDP/registration (existing).
2. POST firmware-check URL with body above.
3. Parse JSON.
   - `success != true` or missing `data` → phase `failed`, surface `errorMessage`, no flash.
   - `updateAvailable == false` **or** stripped `currentVersion` equals `latestVersion` **or** empty `presignedUrl` → phase `no_update`.
4. Require `sha256` (64 hex) and `size > 0`. Else `failed`.
5. Map: `version=latestVersion`, `url=presignedUrl`, `sha256`, `size`.
6. If not `force` and `latestVersion` equals NVS `ota_applied`, `no_update` (same loop-guard as today).
7. `fw_ota_begin(size, sha256)` → `net_lte_http_get_stream(presignedUrl)` immediately (URL TTL ~600 s) → `fw_ota_write` → `fw_ota_end_and_reboot`.
8. Boot confirm/rollback unchanged.

Presigned GET is headerless (S3 signature is in the query string).

## Config

| Source | Role |
|---|---|
| Kconfig `FW_OTA_LTE_CHECK_URL` | Factory firmware-check POST URL (QA Trafyn default above) |
| NVS `ota_manif` | Optional override of that URL (empty → Kconfig). Rename in docs/UI to “firmware check URL”; keep key so existing NVS still works |
| Kconfig `FW_OTA_LTE_AUTH_HEADER` | Optional `Authorization` value, e.g. `Bearer …`; empty = omit |
| Kconfig `FW_OTA_LTE_SYSTEM_USER_ID` | Optional `x-nc-system-user-id`; empty = omit |
| Kconfig manufacturer / device type | Defaults above |
| NVS `uplink_did` | `deviceId` |
| NVS `ota_applied` | Last successfully applied `latestVersion` |
| NVS `ota_force` | SoftAP/serial only; auto-check ignores |
| NVS `ota_chan` | Unused for Trafyn (not in POST body). Leave in NVS; do not send |

Remove Kconfig `FW_OTA_LTE_DEFAULT_MANIFEST_URL` ngrok default.

## SoftAP / serial

Keep:

- SoftAP status: CAN, LTE, uplink, OTA **phase**, running version, stripped currentVersion, last `latestVersion`, `updateAvailable`, HTTP status, error, firmware-check URL (read-only)
- Edit `device_id`
- Serial `lte`, `status`; trigger check (`ota lte` / existing run command) against Trafyn, not ngrok

Remove:

- `POST /api/ota` firmware upload handler and UI uploader
- Manifest GET URL / channel editors that exist only for ngrok
- Any ngrok host baked into defaults, comments, or UI copy

## `net_lte`

Extend HTTPS POST so it can send the two optional headers when configured. Empty config must produce today’s no-custom-header POST (QA no-auth). GET stream unchanged.

## Failures

| Case | Phase | Flash? |
|---|---|---|
| LTE down / POST timeout | `failed` | no; retry next interval |
| HTTP non-2xx or `success: false` | `failed` | no |
| No `sha256`/`size`/`presignedUrl` | `failed` | no |
| Stream GET fail or hash/size mismatch | `failed` | abort slot, no reboot |
| Presigned URL expired | `failed` | no; next check issues a new URL |

## Remove from tree (ngrok / lab GET)

- Firmware client for GET `/firmware/manifest`
- Default ngrok manifest URL in `sdkconfig.defaults` / Kconfig
- SoftAP `.bin` upload path
- `tools/ota_dev_server/` (lab GET server; unused once Trafyn is the only check API)

Historical specs under `docs/superpowers/specs/2026-08-03-*ota*` stay as history; this spec is the live OTA check contract.

## Testing (device)

1. QA POST with `updateAvailable: false` → `no_update`, no reboot.
2. QA POST with newer `latestVersion` + valid `sha256`/`size` + live presigned URL → download, other slot, reboot, confirm.
3. Truncated or wrong hash → `failed`, still running previous slot.
4. Empty auth Kconfig → POST succeeds on current no-auth QA.
5. SoftAP shows phases; no file upload control; no ngrok URL.
