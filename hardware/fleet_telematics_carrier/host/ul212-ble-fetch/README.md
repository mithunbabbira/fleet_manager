# UL212 BLE Fetch

Standalone ESP32-C6 module that polls a **Tenet UL212** fuel sensor over Bluetooth
(Modbus RTU on `0xFFE0` / `0xFFE1`) and forwards readings to the fleet carrier over
**Zigbee** (`fleet_tlv` on cluster `0xFC00`).

**Provision from your computer** — USB serial only. No Wi‑Fi or web UI on the ESP32.

## Quick start

```bash
cd ul212-ble-fetch
pio run -t upload --upload-port /dev/cu.usbmodem1201
pip install -r tools/host_console/requirements.txt
python3 tools/host_console/app.py --port /dev/cu.usbmodem1201
```

Opens **http://127.0.0.1:8765/** — scan BLE, set MAC + device_id, save to NVS.

On the **carrier** USB serial: `fleet hosts` → should show your `device_id` and `height_mm`.

## Host Console (recommended)

| Step | Action |
|------|--------|
| 1 | Plug host ESP32 via USB |
| 2 | Run `python3 tools/host_console/app.py` |
| 3 | Connect serial port in the browser |
| 4 | Scan → pick UL212 MAC → set device_id → Apply & Save |

See `tools/host_console/README.md`.

## Serial commands (optional)

You can also use `pio device monitor -b 115200` and type `help`, `scan`, `mac`, `id`, `save`.

Scripting: `tools/provision_serial.py` (same commands, no browser).

## Zigbee build

`platformio.ini` sets `board_build.zigbee_mode = ed`, `FLEET_ZIGBEE_ED_RADIO=1`, `partitions_zigbee.csv`.

Carrier must have `CONFIG_FLEET_ZIGBEE_ENABLE=y` (channel 15, open join).

## Layout

```
ul212-ble-fetch/
├── src/serial_cli.cpp    ← ESP32 serial command handler
├── tools/host_console/   ← PC web app (serial bridge)
└── tools/provision_serial.py
```

## Notes

- Runtime uses **BLE + Zigbee only** — Wi‑Fi is not started.
- Close the vendor phone app before pairing (one BLE master at a time).
