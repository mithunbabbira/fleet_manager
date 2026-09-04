# Fleet Zigbee host guide

How to add a sensor host so the carrier needs **no schema table changes** for cloud uplink.

## Architecture

```
Host (ESP32-C6)                    Master carrier (ESP32-C6)
  sensor → TLV encode                Zigbee coordinator (open network)
         → HELLO (envelope+map) ──►  → host_registry (dynamic slots)
         → REPORT (values)      ──►  → telemetry_uplink → LTE → cloud
```

Host owns cloud envelope fields on every HELLO/REPORT:

| Field | TLV | Notes |
|-------|-----|--------|
| `device_id` | 1 | Stable host identity |
| `node_id` | 8 | Cloud `node_id` |
| `schemaId` | 9 | Cloud `schemaId` (e.g. `1088`) |
| `host_type` | 10 | Optional display / payload |
| metric map | body 11 | `tlv_id:key:unit:type;…` on HELLO |

Carrier still uses `host.manifest.json` as an optional catalog for lab/legacy hosts. New hosts can join with envelope + metric map only.

## Add a new host

1. Copy `host/_template/` to `host/my-sensor/`.
2. Set on the host firmware (NVS / defaults): `device_id`, `node_id`, `schema_id`, `host_type`, and HELLO `metricMap`.
3. Optionally add `host.manifest.json` and run `python3 scripts/gen_fleet_manifests.py` for carrier-side documentation / legacy key mapping.
4. Map sensor readings to the same `tlv_id`s as the metric map; call `fleetZigbeeEdSendReport()`.
5. Flash the **host**. Flash the **master** only if you changed shared protocol code (`fleet_tlv`) — not for a new schema id alone.

## Provision and verify

**Master (USB serial, 115200)**

| Command | Purpose |
|---------|---------|
| `fleet hosts` | Registry snapshot (`device_id`, `node_id`, `schema_id`, readings) |
| `fleet ingest <hex>` | Loopback test without RF |
| `uplink` | SD queue depth, last produce/drain status |

**Host (USB serial, 115200)**

| Command | Purpose |
|---------|---------|
| `help` | Serial command list |
| `scan` | Find UL212 BLE MAC |
| `mac AA:BB:…` | Set sensor MAC |
| `id ul212-001` | Set Zigbee `device_id` (refreshes default `node_id`) |
| `node node-…` | Set cloud `node_id` |
| `schema 1088` | Set cloud `schemaId` |
| `save` | Persist NVS and reboot |
| `status` | BLE reading + Zigbee join |

Or use `host/ul212-ble-fetch/tools/host_console/` — local web UI on the PC (serial bridge).

Look for `[zb] joined, HELLO sent` and steady `[UL212]` lines.

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/fleet/hosts` | GET | Same data as `fleet hosts` (join carrier SoftAP `Fleet-C6`) |
| `/api/fleet/zigbee` | GET | `enabled`, `channel`, `running` |

## Enable Zigbee radio

**Master** — `sdkconfig.defaults` or menuconfig → **Fleet Zigbee** → `CONFIG_FLEET_ZIGBEE_ENABLE=y`, channel 15.

**Host** — in `platformio.ini`:

```ini
board_build.zigbee_mode = ed
build_flags = -DFLEET_ZIGBEE_ED_RADIO=1 -DZIGBEE_MODE_ED
board_build.partitions = partitions_zigbee.csv
```

When the coordinator flag is off, use `fleet ingest` on the master for bench tests.

## Reference: UL212 BLE fetch

| Item | Path |
|------|------|
| Manifest | `host/ul212-ble-fetch/host.manifest.json` |
| Report task | `host/ul212-ble-fetch/src/zigbee_report.cpp` |
| Zigbee ED | `host/lib/FleetZigbee/src/fleet_zigbee_ed.cpp` |
| Protocol | `host/lib/FleetProtocol` (keep in sync with `components/fleet_protocol`) |

Defaults: `device_id=ul212-001`, `node_id=node-ul212-001`, `schema_id=1088`.

## Unit tests

```bash
cd tests/host && cmake -B build . && cmake --build build && ctest --test-dir build
```

- `test_fleet_tlv` — encode/decode + envelope/metric map
- `test_host_registry` — legacy catalog + dynamic envelope ingest
- `test_fleet_uplink_path` — registry → host-owned `schemaId` JSON

## Design

See `docs/superpowers/specs/2026-09-03-host-owned-zigbee-envelope-design.md`.
