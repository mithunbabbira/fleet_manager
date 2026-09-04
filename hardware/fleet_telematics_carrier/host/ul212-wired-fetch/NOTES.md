# UL212 Wired RS-232 + BLE — What Worked

**Date:** 2026-09-04  
**Hardware:** ESP32-C3 / CH340 USB-TTL → MAX232 → Tenet UL212 (RS-232)  
**App:** TankOffline (BLE), password `52381`

This note records what actually worked in lab testing. Some vendor Hycomprog/SSCOM steps only worked after TankOffline protocol/address settings matched the wired command.

---

## 1. Vendor summary (send this)

### What failed at first

1. Treating the wired UART like the BLE Modbus path (`09 03 00 FD…` unlock/poll) — **no reply**.
2. Sending Hycomprog hex `$!RY0151` while the sensor was still on the wrong **protocol / address** in the app — **no reply**.
3. Weak / wrong RS-232 drive on sensor RX (~**+1.64 V** idle) — sensor cannot hear polls. Working idle on sensor RX was ~**−4.3 V** after MAX232 T1OUT was connected correctly.
4. MAX232 path must be proven with **T1OUT ↔ R1IN loopback** before blaming the sensor.

### What worked (wired)

| Item | Working value |
|------|----------------|
| Physical link | USB → CH340 (TTL) → **MAX232** → UL212 RS-232 |
| Baud | **9600 8N1** |
| TankOffline **address** | **1** |
| TankOffline **protocol** (height-only, pollable ~1 Hz) | **51** |
| TankOffline **protocol** (height + temp + tilt + signal) | **14** |
| Hycomprog “protocol” in the tool UI | **12** (tool mode only — not the same as sensor protocol 51/14) |
| Poll command (address 1, protocol 51) | `24 21 52 59 30 31 35 31 0D 0A` = `$!RY0151` + CR LF |
| Poll / wake (address 1, protocol 14) | `24 21 52 59 30 31 31 34 0D 0A` = `$!RY0114` + CR LF |
| App password | **52381** |

**Wiring (confirmed):**

```
Sensor TX (−5.6 V idle) → MAX232 R1IN (pin 13)
MAX232 T1OUT (pin 14)   → Sensor RX (idle ~−4…−9 V when driven)
CH340 TX                → MAX232 T1IN (pin 11)
MAX232 R1OUT (pin 12)   → CH340 RX
GND common: sensor + MAX232 + CH340
MAX232 VCC: 5 V if chip is MAX232 (use MAX3232 if only 3.3 V available)
```

**Loopback proof (no sensor):** short **T1OUT ↔ R1IN**; type/send bytes at 9600 — must echo. Ours: PASS.

### Example replies

**Protocol 51 (height only):**

```text
TX: $!RY0151\r\n
RX: *CFV0100016CA4\r\n
```

- `01` = address  
- `00016` = raw height → **1.6 mm** (÷10; 0.1 mm units)  
- No tilt / temp / signal in this frame  

**Protocol 14 (full fields):**

```text
TX: $!RY0114\r\n   (or sensor auto-sends on its RS-232 interval)
RX: *XD,4850,20,0364,2300,0363,0651,1005#
```

Approximate decode (Tenet UL202/UL212 protocol 14 style):

| Field | Example | Meaning |
|-------|---------|---------|
| Smooth height | `0364` | 36.4 mm |
| Signal (+ codes) | `2300` | signal ≈ 23 (app prefers ≥ 60) |
| Real-time height | `0363` | 36.3 mm |
| Temperature | `0651` | (651−400)×0.1 = **25.1 °C** |
| Tilt (last hex byte) | `…05` | **5°** |

**Note:** Protocol 14 often pushes ~every **8–10 s** (GPS-tracker style). BLE app stays ~**1 s**. Protocol 51 answers **every poll** (~1 Hz) but height only.

### Ask vendor to clarify

1. Document that **Hycomprog protocol 12** ≠ **sensor protocol 51/14**; TankOffline must set **address 1** and the matching **sensor protocol** before SSCOM hex works.  
2. Publish the **RS-232 output / report interval** for protocol 14 (how to set **1 s** like BLE).  
3. Confirm whether **tilt / temp / signal** are available on any pollable protocol other than 14’s slow push.  
4. Confirm MAX232 levels: sensor RX must see real RS-232 MARK (~−5 V), not ~+1.6 V.

---

## 2. BLE (app / our `ul212-ble-fetch`) — what works

BLE is a **different stack** from wired ASCII `$!RY…`.

| Item | Working value |
|------|----------------|
| Service | `0xFFE0` |
| Notify / write | `0xFFE1` / `0xFFE2` (write may fall back to FFE1) |
| App password | **52381** (= unlock word `0xCC9D`) |
| Unlock (primary) | `09 06 01 07 CC 9D AC 16` |
| Unlock (alt) | `09 06 01 07 7D 5A 99 D4` |
| Poll | `09 03 00 FD 00 1C D4 BB` (Modbus RTU, ~62-byte reply) |
| Typical poll period | **~1 s** |
| Readings we decode | height, smooth height, temp, signal, valid echo, **tilt** |

Firmware: `hardware/fleet_telematics_carrier/host/ul212-ble-fetch/`.

**Do not** expect BLE Modbus frames on the RS-232 pins. Wired uses `$!RY…` / `*CFV` / `*XD`.

---

## 3. Lab tools in this repo

| Tool | Path / command |
|------|----------------|
| Live protocol 14 listener | `ul212-wired-fetch/poll_ul212.py` |
| ~1 Hz height (protocol 51) | `python3 poll_ul212.py --fast51 /dev/cu.usbserial-XXX` |
| Minimal ESP32-C3 UART sketch | `ul212-wired-fetch/src/main.cpp` (was Modbus; update to `$!RY0114` before production use) |

Example:

```bash
cd hardware/fleet_telematics_carrier/host/ul212-wired-fetch
python3 poll_ul212.py /dev/cu.usbserial-110          # protocol 14 listen
python3 poll_ul212.py --fast51 /dev/cu.usbserial-110 # needs app protocol 51
```

---

## 4. Checklist before calling a sensor “dead”

1. [ ] MAX232 **T1OUT↔R1IN** loopback PASS (CH340 or ESP32)  
2. [ ] Sensor TX idle ~**−5.6 V** on R1IN  
3. [ ] Sensor RX idle ~**−4…−9 V** with T1OUT connected  
4. [ ] Common GND  
5. [ ] TankOffline: **address 1**, protocol **51** or **14** as intended  
6. [ ] Send matching `$!RY01xx` hex — not BLE Modbus  

---

## 5. Bottom line

| Goal | Use |
|------|-----|
| Wire proof + height only, fast | Protocol **51** + `$!RY0151` |
| Wire: height + temp + tilt + signal | Protocol **14** + `*XD` (often slow push) |
| Live tilt / rich data like phone | **BLE** Modbus unlock `52381` / `CC9D` |

Vendor doc alone (Hycomprog 12 + hex) is incomplete without stating that TankOffline **sensor protocol** and **address** must match the payload (`01` + `51` or `14`).
