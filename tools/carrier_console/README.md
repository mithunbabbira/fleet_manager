# Fleet Carrier Console

Browser UI on your PC for configuring the **carrier ESP32-C6** over USB serial. The firmware no longer runs SoftAP or on-device HTTP (saves flash; avoids Wi-Fi/Zigbee contention).

## Setup

From the repo root:

```bash
python3 -m pip install -r tools/carrier_console/requirements.txt
python3 tools/carrier_console/app.py
# or
./tools/carrier_console/run.sh
```

Auto-connect a port:

```bash
python3 tools/carrier_console/app.py --port /dev/cu.usbmodem1101
```

Open **http://127.0.0.1:8766** (Host Console uses **8765**).

The **Connected hosts** panel parses `fleet hosts` output automatically (refreshes every 5s while serial is connected). Hosts go **offline** if no Zigbee report arrives for 5s (UI uses carrier uptime vs `last_seen_ms`; firmware 1.0.11+ also clears `link_ok` on snapshot). The **Carrier** column shows firmware, LTE, Zigbee activity, and uplink state from serial responses.

## What you can configure

| Area | Serial commands (via UI) |
|------|--------------------------|
| Status / metrics | `status`, `metrics` |
| Uplink | enable, interval, device_id, node_id, send now, queue test |
| LTE OTA | force, device_id, manifest URL, run check |
| LTE modem | status, reconnect, self-test |
| OBD profile | list, set active |
| OBD / DTC / VIN | raw cmd, VIN, DTC read/clear |
| Safety | allow_unsafe (Mode 04 gate) |
| Fleet Zigbee | `fleet hosts` |

Raw console: type commands in any serial terminal at 115200 baud (`help` on device).

## Quit

**Quit app** stops the Python server only; the ESP32 keeps running.
