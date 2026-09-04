# How to make a host (Zigbee → master)

This guide is for sensor hosts (e.g. UL212 over RS-232 or BLE) that join the **carrier master** over Zigbee and show up in cloud uplink as schema **1088**.

Milestone 1 ships this document only. Host firmware still lives under `hardware/fleet_telematics_carrier/host/` until a later milestone ports it here.

## Rules that keep trucks from mixing

1. **Zigbee channel** on the host must match **that truck’s** master (`CONFIG_FLEET_ZIGBEE_CHANNEL`, default **15**). Nearby trucks use **different** channels.
2. **`device_id`** must be unique per host board (e.g. `ul212-rs232-001`). Same id on two boards collides in the registry/cloud.
3. **`node_id`** is usually `node-<device_id>`.
4. **`schema_id`** for UL212-style fuel hosts is **`1088`**.

Channel alone is RF isolation. `device_id` is application identity after join.

## What the master expects on the wire

1. Join the open Zigbee network on the configured channel.
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
- [ ] Channel matches this truck’s master  
- [ ] HELLO metric map matches REPORT reading IDs  
- [ ] No Bluetooth required for wired RS-232 hosts  
- [ ] USB log shows join + periodic reports; master `fleet hosts` lists the device  

## Reference implementations (legacy tree)

- Wired RS-232 + Zigbee: `hardware/fleet_telematics_carrier/host/ul212-rs232-fetch/`  
- BLE + Zigbee: `hardware/fleet_telematics_carrier/host/ul212-ble-fetch/`  
- Shared ED stack: `hardware/fleet_telematics_carrier/host/lib/FleetZigbee/`
