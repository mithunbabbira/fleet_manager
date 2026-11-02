# Fleet cabin camera lab (Phase 1–2)

Throwaway lab under `firmware_v2/camera/`. **Not** wired into `fleet_v2_master` yet.

Branch: `feat/cabin-camera-lab`

## Architecture (Phase 2)

```
Laptop/phone ──Wi‑Fi──► Master C6 SoftAP (webserver) ──Wi‑Fi──► ESP32-CAM + MG996R
   ?deg=90                    192.168.4.1                      STA 192.168.4.10
                                     │                                │
                                     └── GET /capture?deg=90 ─────────┘
                                              move → settle → JPEG
```

Yes: **the master runs SoftAP + an HTTP webserver**. The CAM keeps `/capture`; the master **proxies** and forwards `deg`.

**Lab server job (DELETE ME):** MQTT/USB `job` → capture → HTTP POST to
[`tools/cam_lab_api/`](../../tools/cam_lab_api/) (see that README). Production
`fleet_v2_master` is untouched.

Production SoftAP was removed from fleet master earlier — this lab uses a **separate** C6 sketch (`master_lab/`) until we port the pattern into `fleet_v2_master`.

| Device | SSID | IP | Role |
|--------|------|-----|------|
| Master lab C6 | **Fleet-C6** / `fleetc61` | `192.168.4.1` | SoftAP + webserver + lab job |
| ESP32-CAM | joins Fleet-C6 | `192.168.4.10` (static) | `/capture` + servo |
| MG996R | — | — | Signal GPIO13; **5 V external** + common GND |
| Your Mac | joins Fleet-C6 | DHCP (often `.2`) | `cam_lab_api` + optional Mosquitto |

### MG996R wiring (important)

| Servo wire | Connect to |
|------------|------------|
| Brown/Black (GND) | ESP32-CAM **GND** and 5 V supply GND |
| Red (VCC) | **External 5 V** (≥2 A recommended) — **not** CAM 3.3 V |
| Orange/Yellow (PWM) | ESP32-CAM **GPIO 13** (change via `-DCAM_SERVO_PIN=`) |

Do not power the MG996R from the FTDI/ESP 3.3 V rail — it will brown out.

## PlatformIO path

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"
```

## 1) Flash master lab (ESP32-C6)

USB JTAG port example: `/dev/cu.usbmodem1101`

```bash
cd firmware_v2/camera/master_lab
pio run -t upload --upload-port /dev/cu.usbmodem1101
pio device monitor -b 115200 --port /dev/cu.usbmodem1101
```

Expect SoftAP `Fleet-C6` and `HTTP up`.

## 2) Flash CAM as STA (join master)

FTDI example: `/dev/cu.usbserial-FTB6SPL3` — IO0→GND for download, then release + reset.

```bash
cd firmware_v2/camera
pio run -e sta_master -t upload --upload-port /dev/cu.usbserial-FTB6SPL3
pio device monitor -b 115200 -e sta_master --port /dev/cu.usbserial-FTB6SPL3
```

Expect `STA ok ip=192.168.4.10`.

## 3) Trigger photo (± servo angle)

Join Wi‑Fi **Fleet-C6** / **fleetc61**, then:

```bash
curl http://192.168.4.1/status
curl http://192.168.4.1/api/cam/ping

# Move to 90°, wait ~800 ms, then one JPEG:
curl -o shot.jpg 'http://192.168.4.1/api/cam/capture?deg=90'
open shot.jpg

# Move only (no photo):
curl 'http://192.168.4.1/api/cam/servo?deg=45'

# Photo at current angle (no move):
curl -o shot.jpg http://192.168.4.1/api/cam/capture
```

`deg` is **0–180** (clamped). After a move, the CAM waits for travel + damp before JPEG:
**1000 ms** base + **8 ms/deg** of travel (`CAM_SERVO_SETTLE_MS` / `CAM_SERVO_MS_PER_DEG`) —
e.g. 90° ≈ 1.7 s, 180° ≈ 2.4 s. Tune in `platformio.ini` if still blurry.

## Phase 1 fallback (CAM alone SoftAP)

```bash
cd firmware_v2/camera
pio run -e softap_lab -t upload --upload-port /dev/cu.usbserial-FTB6SPL3
```

Join **Fleet-CAM** / **fleetcams**:

```bash
curl -o shot.jpg 'http://192.168.4.1/capture?deg=90'
```

## Quality

UXGA 1600×1200, JPEG quality 10. No `/stream`.

## Later (not done)

- Port SoftAP + `/api/cam/capture?deg=` into `fleet_v2_master` (Kconfig)
- Save JPEG to SD; replace `tools/cam_lab_api` with real server jobs
- EC200U MQTT trigger — see `tools/cam_lab_api/ec200u_mqtt_phase2.md`
