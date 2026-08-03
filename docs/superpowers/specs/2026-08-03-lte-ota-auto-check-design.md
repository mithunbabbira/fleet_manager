# LTE OTA auto-check (production-like)

**Status:** approved  
**Depends on:** `fw_ota_lte` (manual SoftAP/serial trigger already proven)

## Goal

After boot, once cellular is usable, the device **automatically** GETs the manifest and updates if `version` differs from the running app. No SoftAP typing required for normal operation.

## Config

| Source | Role |
|--------|------|
| Kconfig `FW_OTA_LTE_DEFAULT_MANIFEST_URL` | Factory default (lab: ngrok; later: CM) |
| NVS `ota_manif` | SoftAP / `ota url` override; wins if non-empty |
| Channel / device_id | Existing NVS + defaults (`stable`, `fleet-demo-001`) |

Auto-check always runs with **force = false** (Force remains SoftAP/serial only).

## Runtime

1. `fw_ota_lte_start_auto()` spawns a low-priority task (if LTE enabled).
2. Wait until `net_lte` reports registered (poll) or ~90s elapsed.
3. Call `fw_ota_lte_start_background()` once (skip if URL empty or busy).
4. Sleep **24h**, repeat step 2–3.

SoftAP Run / lab buttons unchanged.

## Out of scope

Signed manifests, cloud push, rate-limit API, changing SoftAP UI beyond status visibility.
