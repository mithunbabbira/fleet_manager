# Fleet Zigbee / Wi-Fi / BLE coexistence

ESP32-C6 boards share one 2.4 GHz radio. Plan RF usage deliberately.

## Radio layout

| Board | Radios | Steady-state |
|-------|--------|--------------|
| Master carrier | Wi-Fi SoftAP (optional), Zigbee coordinator, LTE UART | Zigbee + LTE; SoftAP optional for bench |
| UL212 host | BLE (UL212), Zigbee ED | **BLE + Zigbee only** — no Wi-Fi |

## Channel plan

| Radio | Channel |
|-------|---------|
| Zigbee (both boards) | 15 (`CONFIG_FLEET_ZIGBEE_CHANNEL`) |
| Master SoftAP (optional) | 1 (`Fleet-C6`) |

## Bench procedure (validated)

1. Flash master with `CONFIG_FLEET_ZIGBEE_ENABLE=y`.
2. Flash host with `FLEET_ZIGBEE_ED_RADIO=1`.
3. Provision host over **USB serial** (`scan`, `mac`, `id`, `save`) or `tools/provision_serial.py`.
4. Power master first (forms network, permit join stays open).
5. Power host — serial: `[zb] joined, HELLO sent` and `[UL212]` lines.
6. On master serial: `fleet hosts` → `ul212-001` with `height_mm` updating.

**Auto-rejoin:** Either board can reboot or lose power. The carrier re-opens permit join every 30 s; the host restarts Zigbee steering when disconnected. No manual re-pairing needed if both stay on channel 15.

## Throughput

~1 Hz × ~80 B TLV frames per host — well under Zigbee duty cycle for a small fleet.

## Recommendations

- Provision each host with USB serial — see `host/ul212-ble-fetch/README.md`.
- Master SoftAP is optional; USB console (`fleet hosts`, `uplink`) is enough for lab work.
- See `docs/fleet-zigbee-host-guide.md` for the full onboarding checklist.
