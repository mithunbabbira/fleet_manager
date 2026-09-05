# Zigbee EPAN Hardening — Design

**Date:** 2026-09-05  
**Status:** Approved (design); awaiting implementation plan  
**Depends on:** M5 Zigbee coordinator + UL212 RS-232 Zigbee Phase 2  
**Supersedes (partial):** channel-only multi-truck guidance in `2026-09-04-ul212-rs232-zigbee-design.md` — trucks now use **channel + Extended PAN ID**

## Goal

Simple truck isolation without install codes or join-window CLI:

1. Hosts do not join the wrong truck’s carrier.
2. Boards flashed without this truck’s EPAN do not join this carrier.

Config stays “edit one place per side + rebuild.” Permit-join stays **forever-open** (current behavior).

## Non-goals

- Zigbee install codes / TC link-key UX
- Timed permit-join / `zb permit` CLI
- Runtime NVS change of EPAN or channel
- App / QR provisioning
- Closing the open network after boot

## Approach

**Fixed Extended PAN ID (64-bit) + channel**, shared on carrier and host.

- Carrier forms the network with the configured EPAN (not a random EPAN).
- Host steers / joins only a network matching that EPAN on the configured channel.
- Permit-join remains always open (255s + periodic refresh), as today.

EPAN is a shared network identifier, not install-code crypto. Anyone with the same channel+EPAN firmware config can join while the network is open — that is the accepted simplicity trade-off.

## Config model

| Side | Knob | Where |
|------|------|--------|
| Carrier | Channel | `CONFIG_FLEET_ZIGBEE_CHANNEL` (existing) |
| Carrier | Extended PAN ID | New `CONFIG_FLEET_ZIGBEE_EPAN_ID` (64-bit; Kconfig + `sdkconfig.defaults`) |
| Host | Channel | `FLEET_ZB_CHANNEL` in `zigbee_app_config.h` (existing) |
| Host | Extended PAN ID | New `FLEET_ZB_EPAN_ID` in `zigbee_app_config.h` |

**Truck template:** unique **channel and** unique **EPAN** per truck.  
**Lab default:** channel `15` + one fixed documented lab EPAN (same on master defaults and host header).

`device_id` / `node_id` remain unique per board for registry and cloud (unchanged).

## Behavior

### Carrier

- Before network formation, set the configured Extended PAN ID.
- Form / bring up on `CONFIG_FLEET_ZIGBEE_CHANNEL`.
- Keep forever-open permit-join (no change to open/refresh policy).
- On network up, log short PAN, EPAN, and channel.

### Host

- Use the same channel and EPAN from `zigbee_app_config.h`.
- Join / steer only toward a matching EPAN on that channel.
- Wrong EPAN or wrong truck → no join.
- Already-joined hosts continue to work across reboots without extra steps.

## Files (expected)

**Carrier**

- `firmware_v2/master/components/transport_zigbee/Kconfig`
- `firmware_v2/master/sdkconfig.defaults` (and local `sdkconfig` as needed)
- `firmware_v2/master/components/transport_zigbee/transport_zigbee_radio.c`
- Comments / host HOWTO if they still say channel-only

**Host**

- `hardware/fleet_telematics_carrier/host/ul212-rs232-fetch/include/zigbee_app_config.h`
- FleetZigbee ED join path under that host (as needed to pass preferred EPAN)

**Unchanged**

- Uplink / mock / ngrok flow
- Install-code policy (`false`)
- TLV / 1088 schema

## Success criteria

1. Matching channel+EPAN on master and host → host joins; `fleet hosts` shows link up.
2. Host rebuilt with a different EPAN (same channel) → does not join this carrier.
3. Lab uplink: mock + ngrok; `uplink url` → ngrok; batch includes host 1088 (and GPS 1089 if available).

## Follow-ups (out of scope)

- Install codes if stronger B is required later
- Timed permit-join if open networks become an operational concern
- Spec touch-up on older Zigbee docs to point here for multi-truck isolation
