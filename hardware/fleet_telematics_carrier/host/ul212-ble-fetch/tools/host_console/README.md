# UL212 Host Console

Local web UI on your computer — talks to the host ESP32 over **USB serial only**.
The ESP32 does not serve a web page or Wi‑Fi.

## Setup

```bash
cd hardware/fleet_telematics_carrier/host/ul212-ble-fetch
pip install -r tools/host_console/requirements.txt
```

## Run

```bash
python3 tools/host_console/app.py
```

Opens **http://127.0.0.1:8765/** in your browser.

Use **Quit app** in the UI (top right) to stop the server — no Ctrl+C needed. This closes the PC app only; the ESP32 is not rebooted.

Auto-connect a known port:

```bash
python3 tools/host_console/app.py --port /dev/cu.usbmodem1201
```

## Workflow

1. Plug host ESP32-C6 via USB.
2. Click **Refresh** → pick serial port → **Connect**.
3. **Scan BLE** → click a listed MAC (fills MAC, saves to NVS, reboots). Or edit fields and **Apply & Save**.
4. On carrier USB serial: `fleet hosts` → confirm readings.

## Architecture

```
Browser (localhost) ←→ Python app (serial bridge) ←→ ESP32 USB serial
                                                      BLE + Zigbee only
```
