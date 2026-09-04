# Firmware v2 — Milestone 1 Design

**Date:** 2026-09-04  
**Status:** Approved for implementation  
**Branch:** `firmware-v2`  
**Tree:** `firmware_v2/` (old `components/` is reference only)

## Goal

Fresh master app: boot + dual-bank OTA over LTE using Trafyn **get-latest-device-firmware** only. Clean modules, simple names, PCB pins unchanged. Host folder: guide document only.

## Non-goals (M1)

- GPS, wall clock, SD queue, OBD/MCP, Zigbee, telemetry JSON uplink, SoftAP
- Trafyn **publish** multipart API in firmware (lab curl only for testers)
- Editing or deleting old `components/` / existing hosts

## Pins (frozen — soldered PCB)

| Function | GPIO | Note |
|----------|------|------|
| LTE ESP TX | **16** | → modem RX |
| LTE ESP RX | **17** | ← modem TX |
| MCP SCK/MOSI/MISO/CS/INT | 21/22/23/20/14 | later milestones, same numbers |
| microSD SCK/MOSI/MISO/CS | 4/5/6/18 | later milestones |

Do not swap LTE TX/RX.

## Layout

```
firmware_v2/
  README.md
  host/HOW_TO_MAKE_A_HOST.md
  master/          # ESP-IDF project
    modules/board, lte, ota, cli
    main/main.c
    partitions.csv
    docs/MASTER.md
```

## OTA contract (device)

**POST** `https://api.trafyn.info/workflow-engine/realm/1/user/1/v1/execution/service/runWithNoAuth/get-latest-device-firmware?refreshCache=true`

Body:

```json
{
  "input": {
    "deviceId": "<from NVS/config>",
    "manufacturer": "Espressif Systems",
    "deviceType": "fleet monitor",
    "currentVersion": "<running app version stripped>"
  }
}
```

Headers: `Content-Type: application/json`; optional `Authorization`, `x-nc-system-user-id` from NVS.

Use `data.updateAvailable`, `presignedUrl`, `sha256`, `size`, `latestVersion`. Stream GET to inactive OTA bank; verify; reboot.

**Not in firmware:** `…/execution/request/multipart` (publish).

## Config

sdkconfig defaults + NVS via USB: `device_id`, `ota_token`, `ota_user`, `save`, `ota check`, `status`.

## Roadmap (after M1)

GPS/time → SD store-on-fail → OBD → Zigbee dynamic hosts + JSON 1087/1088/1089 envelope unchanged.

## Success

1. Flash master; USB shows version + `ota_0`/`ota_1`
2. LTE up on GPIO16/17
3. Lab publish newer bin → device updates and boots other bank
4. No publish URL in sources; no old `components/` edits
