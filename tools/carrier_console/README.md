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

The **Connected hosts** panel parses `fleet hosts` output automatically (refreshes every 5s while serial is connected). Hosts go **offline** if no Zigbee report arrives for **20s** (UI uses carrier uptime vs `last_seen_ms`; firmware also clears `link_ok` on snapshot). Host cards show uplink envelope fields (`node=` / `schema=`). The **Carrier** column shows firmware, LTE, Zigbee activity, and uplink state from serial responses. On connect/refresh the console also runs `config`, `ota status`, `lte`, and `uplink` so identity/form fields and Diagnostics path tiles fill without extra clicks. **OBD Active profile** is loaded via `profiles` (NVS PID set — separate from CAN protocol auto-detect).

### OBD protocol vs profile

| Concept | What it is | Auto on OBD plug-in? | Saved? |
|---------|------------|----------------------|--------|
| **Protocol** | ISO 15765-4 CAN 11/29 @ 250/500 (Vehicle link) | Yes — firmware sweeps until ECU answers `0100` | Yes — NVS, reused next boot |
| **Profile** | Named PID poll list (`fleet_basic`, …) | No — not from the connector | Yes — NVS active name; set via **Set profile** |

### Diagnostics

The **Diagnostics** card (below Connected hosts) shows per-host uplink envelope fields (`node_id`, `schema_id`), link freshness, a readings summary, carrier Zigbee radio/report counts, and **Carrier path** tiles (GPS fix, last uplink HTTP, SD queue). Dashboard refresh also runs `uplink` so those lines arrive without a separate command. Use **Copy fleet snapshot** for clipboard JSON (includes `carrier_path`).

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
