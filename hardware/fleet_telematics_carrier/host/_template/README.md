# Fleet host template

Copy this folder when adding a new Zigbee host device.

1. Duplicate `_template/` to a new name (e.g. `my-sensor-host/`).
2. Edit `host.manifest.json`: unique `host_type`, `host_type_id`, and `readings`.
3. Implement sensor glue and call `fleetZigbeeEdSendReport()` from your poll loop (~1 Hz).
4. Rebuild the main carrier firmware so the manifest catalog picks up the new host.

See `docs/fleet-zigbee-host-guide.md` in the repo root.
