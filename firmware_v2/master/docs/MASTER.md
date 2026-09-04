# Firmware v2 master — module map

| Module | Path | Job |
|--------|------|-----|
| board | `components/board` | Pin macros only (LTE TX=16, RX=17) |
| lte | `components/lte` | Modem bring-up + `lte_http_post` / `lte_http_get_stream` (QHTTP) |
| ota | `components/ota` | Dual-bank flash + Trafyn get-latest cloud OTA |
| cli | `components/cli` | USB commands for device_id / token / ota check |
| main | `main/main.c` | Boot order |

## USB commands

- `help`, `status`
- `gps` (show the latest on-modem GPS fix)
- `device_id <id>`
- `ota_token <value>` / `ota_token` to clear
- `ota_user <id>` / `ota_user` to clear
- `save` (re-persist current config)
- `ota check`

`status` includes `time:` and `gps:` lines. After LTE registration, `time:
ok=yes` reports the synchronized IST wall clock and source (normally `cclk`).
An indoor `gps: ok=no` result is expected until the modem gets a satellite fix.
GPS is provided by the LTE modem, so it requires no additional ESP GPIOs.

## Lab OTA test

1. Build: `idf.py build` → `build/fleet_v2_master.bin`
2. Publish with your multipart curl (not in firmware)
3. On device: set token/user if needed, then `ota check`
4. Device should reboot onto the other OTA bank

The OTA path suspends background modem AT work (including time/GPS polling) for
the check and resumes it afterward, preventing concurrent UART transactions.

Publish URL must never appear in this tree.
