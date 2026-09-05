# Firmware v2

Clean rewrite of the fleet telematics stack. The legacy tree (`components/`, root `main/`, old hosts) stays as **reference** until we delete it later.

## Layout

| Path | Role |
|------|------|
| [`master/`](master/) | ESP-IDF app for the soldered carrier PCB (ESP32-C6) |
| [`host/`](host/) | Rules for building Zigbee sensor hosts that join the master |

## Milestone roadmap

1. **M1 (done):** master boot, dual-bank OTA, LTE HTTP GET/POST, Trafyn get-latest OTA, host HOWTO only  
2. **M2 (done):** on-modem GPS, wall clock, USB `status`/`gps` — see `docs/superpowers/specs/2026-09-04-firmware-v2-milestone2-design.md`  
3. **M3 (done):** uplink core + GPS schema **1089** — see `docs/superpowers/specs/2026-09-04-firmware-v2-milestone3-uplink-design.md`  
4. **M4 (done):** MCP2515 / OBD → schema **1087** — `docs/superpowers/specs/2026-09-04-firmware-v2-milestone4-obd-design.md`  
5. **M5 (done):** Zigbee + dynamic hosts → **1088** — `docs/superpowers/specs/2026-09-04-firmware-v2-milestone5-zigbee-design.md`  
6. **M6 (done):** SD store-on-fail — `docs/superpowers/specs/2026-09-04-firmware-v2-milestone6-sd-design.md`  

JSON envelope (unchanged): top-level `device_id`, `node_id`, `schemaId`, `ts_ms`, `payload`. SD is last — it only stores already-built uplink lines.

## Build master

```bash
cd firmware_v2/master
source ~/esp/esp-idf/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

**Pins (frozen):** LTE UART ESP TX=GPIO16, RX=GPIO17. MCP soft-SPI SCK=21 MOSI=22 MISO=23 CS=20 INT=14. SD SPI2 SCK=4 MOSI=5 MISO=6 CS=18.

## Docs

- M1: `docs/superpowers/specs/2026-09-04-firmware-v2-milestone1-design.md`
- M2: `docs/superpowers/specs/2026-09-04-firmware-v2-milestone2-design.md`
- Master modules: [`master/docs/MASTER.md`](master/docs/MASTER.md)
- Host guide: [`host/HOW_TO_MAKE_A_HOST.md`](host/HOW_TO_MAKE_A_HOST.md)
- Backend contract: `docs/telemetry-api-backend-guide.md`
