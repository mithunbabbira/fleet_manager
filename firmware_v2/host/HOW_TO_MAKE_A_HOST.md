# How to make a host (Zigbee → master)

This guide is for sensor hosts (e.g. UL212 over RS-232 or BLE) that join the **carrier master** over Zigbee and show up in cloud uplink as schema **1088**.

**Master coordinator:** firmware_v2 master includes the Zigbee coordinator (`transport_zigbee`). Set `CONFIG_FLEET_ZIGBEE_CHANNEL` and `CONFIG_FLEET_ZIGBEE_EPAN_ID` on the master (defaults: channel **15**, EPAN **`F1EE700000000001`**) and use the **same channel and EPAN** on every host for that truck.

Host application firmware still lives under `hardware/fleet_telematics_carrier/host/` until a later milestone ports it here.

## Rules that keep trucks from mixing

1. **Zigbee channel and EPAN** on the host must match **that truck’s** master (`CONFIG_FLEET_ZIGBEE_CHANNEL` / `CONFIG_FLEET_ZIGBEE_EPAN_ID`; lab defaults channel **15**, EPAN **`F1EE700000000001`**). Nearby trucks use **different** channel **and** EPAN pairs.
2. **`device_id`** must be unique per host board (e.g. `ul212-rs232-001`). Same id on two boards collides in the registry/cloud.
3. **`node_id`** is usually `node-<device_id>`.
4. **`schema_id`** for UL212-style fuel hosts is **`1088`**.

Channel + EPAN together are RF isolation. `device_id` is application identity after join. After changing EPAN on a host, **erase flash once** so stale Zigbee NVS does not block join. See [`docs/superpowers/specs/2026-09-05-zigbee-epan-hardening-design.md`](../../docs/superpowers/specs/2026-09-05-zigbee-epan-hardening-design.md).

## What the master expects on the wire

1. Join the open Zigbee network on the configured channel and preferred EPAN.
2. Send **HELLO** with envelope fields + metric map (TLV defs), e.g.  
   `16:height_mm:mm:f;17:smooth_mm:mm:f;18:temperature_c:C:f;19:signal::u8;20:valid_echo::u8;21:tilt_deg::u8`
3. Send **REPORT** with the same reading IDs and current values.
4. Master registers the host dynamically and builds uplink JSON **1088** without a hard-coded catalog entry for every new device.

## Cloud JSON shape (do not invent a new envelope)

Identity stays **outside** `payload`:

```json
{
  "device_id": "ul212-rs232-001",
  "node_id": "node-ul212-rs232-001",
  "schemaId": "1088",
  "ts_ms": 0,
  "payload": { }
}
```

Full field list: `docs/telemetry-api-backend-guide.md`.

## Suggested host checklist

- [ ] Unique `device_id` / `node_id` in host config  
- [ ] Channel and EPAN match this truck’s master  
- [ ] HELLO metric map matches REPORT reading IDs  
- [ ] No Bluetooth required for wired RS-232 hosts  
- [ ] USB log shows join + periodic reports; master `fleet hosts` lists the device  

## Reference implementations (legacy tree)

- Wired RS-232 + Zigbee: `hardware/fleet_telematics_carrier/host/ul212-rs232-fetch/`  
- BLE + Zigbee: `hardware/fleet_telematics_carrier/host/ul212-ble-fetch/`  
- Shared ED stack: `hardware/fleet_telematics_carrier/host/lib/FleetZigbee/`
