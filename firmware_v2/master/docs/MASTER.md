# Firmware v2 master — module map

| Module | Path | Job |
|--------|------|-----|
| board | `components/board` | Pin macros (LTE 16/17; MCP 21/22/23/20/14; SD 4/5/6/18) |
| lte | `components/lte` | Modem + QHTTP + GPS/time |
| ota | `components/ota` | Dual-bank flash + Trafyn get-latest |
| can_obd | `components/can_obd` | Soft-SPI MCP2515 + ISO-TP OBD |
| obd_codec | `components/obd_codec` | Mode-01 PID decode |
| obd_poller | `components/obd_poller` | Thin Mode-01 poll (rpm/speed/coolant/throttle) |
| fleet_protocol | `components/fleet_protocol` | HELLO/REPORT TLV codec |
| host_registry | `components/host_registry` | Dynamic Zigbee host registry |
| transport_zigbee | `components/transport_zigbee` | ESP32-C6 Zigbee coordinator (ch 15) |
| store_sd | `components/store_sd` | FatFS NDJSON uplink queue (SPI2) |
| uplink | `components/uplink` | Modular builders + **one tick → one JSON array** (1087/1088/1089); SD store-on-fail |
| cli | `components/cli` | USB commands |
| main | `main/main.c` | Boot order |

## USB commands

- `help`, `status`, `gps`, `can status`, `fleet hosts`, `sd status`
- `device_id <id>` (OTA + uplink carrier id)
- `node_id <id>` (required for OBD/GPS uplink POST)
- `ota_token` / `ota_user` / `save` / `ota check`
- `uplink status` / `uplink once` (one array of whatever is ready) / `uplink test` (synthetic 1089) / `uplink url` (read-only, from bin)
- `fleet hosts` / `fleet demo` (lab inject 1088 without Zigbee radio)

`status` includes `can: mcp=` (chip) + `ready=` (ECU) and a short `obd:` line. `fleet hosts` lists registry snapshot. `sd status` shows mount + queue depth.

## Uplink

- **Fleet URLs (edit one file):** `firmware_v2/master/sdkconfig.defaults`
  - `CONFIG_OTA_CLOUD_CHECK_URL` — firmware check
  - `CONFIG_UPLINK_URL` — telemetry POST (`…/v1/sources/nc-fleet-device/messages`)
  Both are baked into the bin (not NVS). Change → rebuild → OTA.
- Per-device: `device_id` / `node_id` / OTA token+user in **NVS** only.
- **Core tick:** `produce_tick` collects OBD + Zigbee hosts + GPS via separate payload builders, then `uplink_batch_build_array` → **one** HTTPS POST
- **Vehicle wrap:** `CONFIG_UPLINK_VEHICLE_WRAP` default **off** → bare JSON array for `/v1/sources/nc-fleet-device/messages`; `=y` only for legacy `{"Vehicle":[...]}`
- Envelope: `device_id`, `node_id`, optional `device_type` (`obd`/`gps` from config), `schemaId`, `ts_ms`, `payload`
- **device_type:** `CONFIG_UPLINK_DEVICE_TYPE_OBD` / `CONFIG_UPLINK_DEVICE_TYPE_GPS` (Zigbee/fuel omits it)
- **1089 GPS:** virtual ids `{master}_GPS` / `node-{master}_GPS`; move/heartbeat gates (bypassed by `uplink once`)
- **1087 OBD:** carrier `device_id` / `node_id`; included when ≥1 fresh PID (≤15 s) — needs ECU
- **1088 host:** host-owned ids from HELLO/REPORT; flat METRIC_MAP keys; one array element per host (`fleet demo` injects a lab host without radio)
- **SD (M6):** on POST fail → enqueue each envelope; drain task batch POST when LTE recovers

## Zigbee

- `CONFIG_FLEET_ZIGBEE_ENABLE=y`, channel **15** — hosts must match
- **Open network** (permit-join, no install codes) — fine for lab; any nearby host on ch 15 can join
- Custom cluster 0xFC00; dynamic registry
- **Deferred (not implemented):** Zigbee install codes / TC-link key commissioning and unique EPAN per truck — see “hardening” note in project status; needed before multi-truck RF isolation beyond “different channel”

## Build (ESP-IDF)

This tree is built and configured for **ESP-IDF 5.2.3** (`~/esp/esp-idf`).

```bash
source ~/esp/esp-idf/export.sh   # must report ESP-IDF v5.2.3
cd firmware_v2/master
idf.py build
# Prefer Trafyn OTA for device updates. USB flash only for recovery:
# idf.py -p /dev/cu.usbmodemXXXX erase-flash flash
```

Do **not** use `~/.espressif/tools/activate_idf_v6.0.1.sh` for this project — IDF 6 lacks the `json` component this app `REQUIRES`, so `idf.py` configure/flash fails there.

## Lab OTA test

1. Build → `build/fleet_v2_master.bin`
2. Publish with multipart curl (not in firmware)
3. `ota check` → other bank

Publish URL must never appear in this tree.
