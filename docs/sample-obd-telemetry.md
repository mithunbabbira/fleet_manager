# Sample OBD Telemetry Data

Captured from a live vehicle session with ESP32-C6 + MODAXE OBDII BLE adapter.
Use this as reference data when designing fleet uplink (Zigbee / UART / MQTT).

## Session context

| Field | Value |
|-------|--------|
| Adapter | MODAXE OBDII (ELM327 v2.2) |
| BLE address | `46:FC:0D:32:1E:66` (often advertises with no local name) |
| Profile | `can_11_500` |
| Protocol | ISO 15765-4 (CAN 11-bit / 500 kbaud) — `ATDPN=6` |
| ESP status | `ble_connected=yes`, `elm_ready=yes`, poller on |

## Raw ELM responses

```text
ATI   -> ELM327 v2.2
ATDP  -> ISO 15765-4 (CAN 11/500)
ATDPN -> 6
ATRV  -> 13.7V
0100  -> 4100983AA013
010C  -> 410C0C2E          # RPM ≈ 779.5 (idle)
010C  -> 410C0C2F          # RPM ≈ 779.75 (later sample)
010D  -> 410D00            # speed = 0 km/h
0105  -> 41057F            # coolant = 87 °C
```

### Decode formulas

| Signal | Formula | Example |
|--------|---------|---------|
| RPM | `((A<<8)\|B) / 4` | `0C2E` → 779.5 |
| Speed | `A` km/h | `00` → 0 |
| Coolant °C | `A - 40` | `7F` → 87 |
| Voltage | parse `ATRV` text | `13.7V` → 13.7 |

---

## Internal message shape (`telemetry_bus`)

Matches `telemetry_pid_sample_t` in `components/telemetry_bus/include/telemetry_bus.h`.

### PID samples

```json
{
  "type": "pid_sample",
  "cmd": "010C",
  "name": "rpm",
  "raw_hex": "410C0C2E",
  "value": 779.5,
  "unit": "rpm",
  "ts_ms": 1720001234500,
  "ok": true
}
```

```json
{
  "type": "pid_sample",
  "cmd": "010D",
  "name": "speed",
  "raw_hex": "410D00",
  "value": 0,
  "unit": "km/h",
  "ts_ms": 1720001234600,
  "ok": true
}
```

```json
{
  "type": "pid_sample",
  "cmd": "0105",
  "name": "coolant_c",
  "raw_hex": "41057F",
  "value": 87,
  "unit": "C",
  "ts_ms": 1720001235000,
  "ok": true
}
```

```json
{
  "type": "pid_sample",
  "cmd": "ATRV",
  "name": "voltage",
  "raw_hex": "13.7V",
  "value": 13.7,
  "unit": "V",
  "ts_ms": 1720001238000,
  "ok": true
}
```

### Events and errors

```json
{ "type": "elm_event", "kind": "connected", "ts_ms": 1720001230000 }
```

```json
{
  "type": "error",
  "code": "ELM_TIMEOUT",
  "cmd": "010C",
  "message": "no response before timeout",
  "ts_ms": 1720001240000
}
```

---

## Poll cadence (`can_11_500`)

| Signal | Cmd | Interval | Rate |
|--------|-----|----------|------|
| RPM | `010C` | 500 ms | 2 Hz |
| Speed | `010D` | 500 ms | 2 Hz |
| Throttle | `0111` | 1000 ms | 1 Hz |
| Coolant | `0105` | 2000 ms | 0.5 Hz |
| Voltage | `ATRV` | 5000 ms | 0.2 Hz |
| **Total** | | | **~5.7 samples/s** |

---

## Fleet snapshot (recommended Zigbee / master uplink)

Poll OBD fast locally; uplink a compact snapshot ~1 Hz.

```json
{
  "v": 1,
  "node_id": "esp32c6-01",
  "vehicle_id": "fleet-demo-001",
  "ble_peer": "46:FC:0D:32:1E:66",
  "adapter": "MODAXE OBDII",
  "profile": "can_11_500",
  "protocol": "ISO15765-4 CAN11/500",
  "ts_ms": 1720001239000,
  "uptime_s": 29,
  "link": {
    "ble_connected": true,
    "elm_ready": true,
    "poller": "on"
  },
  "metrics": {
    "cmds_ok": 143,
    "cmds_fail": 0,
    "ble_reconnects": 0,
    "blocked_cmds": 0,
    "telemetry_drops": 0
  },
  "samples": [
    { "k": "rpm", "cmd": "010C", "v": 779.5, "u": "rpm", "raw": "410C0C2E", "ok": true, "age_ms": 120 },
    { "k": "speed", "cmd": "010D", "v": 0, "u": "km/h", "raw": "410D00", "ok": true, "age_ms": 180 },
    { "k": "coolant_c", "cmd": "0105", "v": 87, "u": "C", "raw": "41057F", "ok": true, "age_ms": 900 },
    { "k": "throttle_pct", "cmd": "0111", "v": 14.5, "u": "%", "raw": "411125", "ok": true, "age_ms": 400 },
    { "k": "voltage", "cmd": "ATRV", "v": 13.7, "u": "V", "raw": "13.7V", "ok": true, "age_ms": 2100 }
  ]
}
```

> Note: `throttle_pct` in the snapshot is illustrative. RPM, speed, coolant, and voltage were confirmed live on the vehicle.

---

## Compact binary frame sketch (~32–40 bytes)

Prefer binary over JSON on Zigbee (small MTU).

```text
[0]      magic       = 0x4F ('O')
[1]      version     = 1
[2]      flags       = ble_ok | elm_ok | poller_on
[3]      seq
[4..7]   ts_s        uint32 (boot or epoch seconds)
[8..9]   rpm_x4      uint16 = (A<<8)|B   (decode /4 on master)
[10]     speed       uint8  km/h
[11]     coolant_a   uint8  (decode A-40)
[12]     throttle_a  uint8  (decode A*100/255)
[13..14] voltage_cV  uint16 = 1370 for 13.70 V
[15]     status / error code
[16..17] cmds_ok     uint16
[18..19] cmds_fail   uint16
```

Example master decode:

```json
{
  "rpm": 779.5,
  "speed_kmh": 0,
  "coolant_c": 87,
  "voltage_v": 13.7,
  "elm_ready": true
}
```

---

## Bandwidth guidance (per vehicle)

| Uplink style | Approx. rate |
|--------------|--------------|
| Recommended: binary snapshot @ 1 Hz | **~1 kbps** |
| Comfortable: 2 Hz + events | **2–5 kbps** |
| Worst case: every PID sample as full struct | **~8–10 kbps** |

Rule of thumb: poll OBD at 500 ms locally; send Zigbee snapshots at 1 Hz; event-uplink only on disconnect / threshold / fault.

---

## Related code

- Profiles: `components/profile_store/builtin_profiles.h`
- Telemetry structs: `components/telemetry_bus/include/telemetry_bus.h`
- Design: `docs/superpowers/specs/2026-07-09-elm327-esp32c6-design.md`
