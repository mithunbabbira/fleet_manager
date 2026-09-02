# Fleet Zigbee host guide

How to add a sensor host to the fleet telematics carrier without editing coordinator C code for each sensor type.

## Architecture

```
Host (ESP32-C6)                    Master carrier (ESP32-C6)
  sensor → TLV encode                Zigbee coordinator (open network)
         → Zigbee REPORT  ────────►  → host_registry
                                       → telemetry_uplink → LTE → cloud
```

- **Contract:** `host.manifest.json` per host under `hardware/fleet_telematics_carrier/host/`.
- **Shared constants:** `fleet_zigbee_cluster.h` (cluster `0xFC00`, endpoints, command id).
- **Security (lab):** open Zigbee join on a fixed channel — no install codes.

## Add a new host

1. Copy `host/_template/` to `host/my-sensor/`.
2. Edit `host.manifest.json` — unique `host_type`, `host_type_id`, and `readings[]` with stable `tlv_id` values.
3. Regenerate the catalog (also runs on ESP-IDF build):
   ```bash
   python3 scripts/gen_fleet_manifests.py
   ```
4. Implement firmware glue: map sensor readings to manifest `tlv_id`s, call `fleetZigbeeEdSendReport()` ~1 Hz.
5. Rebuild and flash the **master** so `fleet_manifest_catalog.c` includes the new host.
6. Flash the **host** with `FLEET_ZIGBEE_ED_RADIO=1` and `board_build.zigbee_mode = ed`.

## Provision and verify

**Master (USB serial, 115200)**

| Command | Purpose |
|---------|---------|
| `fleet hosts` | Registry snapshot (`device_id`, readings, join state) |
| `fleet ingest <hex>` | Loopback test without RF |
| `uplink` | SD queue depth, last produce/drain status |

**Host (USB serial, 115200)**

| Command | Purpose |
|---------|---------|
| `help` | Serial command list |
| `scan` | Find UL212 BLE MAC |
| `mac AA:BB:…` | Set sensor MAC |
| `id ul212-001` | Set Zigbee device_id |
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
| Protocol | `host/lib/FleetProtocol` |

## Unit tests

```bash
cd tests/host && cmake -B build . && cmake --build build && ctest --test-dir build
```

- `test_fleet_tlv` — encode/decode
- `test_host_registry` — HELLO/REPORT ingest
- `test_fleet_uplink_path` — registry → JSON `hosts[]`

## Known gaps (not device bugs)

- Cloud batch POST may return **HTTP 400** until the backend schema accepts `hosts[]`.
- `report_interval_ms` in the manifest is documentation today; host firmware uses 1 s in `zigbee_report.cpp`.
