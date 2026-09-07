# Firmware v2

Product firmware for the fleet telematics carrier. This is the **only** ESP-IDF
app in the repository.

## Layout

| Path | Role |
|------|------|
| [`master/`](master/) | ESP-IDF app for the soldered carrier PCB (ESP32-C6) |
| [`host/`](host/) | Rules for building Zigbee sensor hosts that join the master |

## Milestone roadmap

1. **M1 (done):** master boot, dual-bank OTA, LTE HTTP GET/POST, Trafyn get-latest OTA, host HOWTO only
2. **M2 (done):** on-modem GPS, wall clock, USB `status`/`gps`
3. **M3 (done):** uplink core + GPS schema **1089**
4. **M4 (done):** MCP2515 / OBD → schema **1087**
5. **M5 (done):** Zigbee + dynamic hosts → **1088**
6. **M6 (done):** SD store-on-fail

## Build master

```bash
cd firmware_v2/master
source ~/esp/esp-idf/export.sh   # ESP-IDF v5.2.3
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

**Pins (frozen):** LTE UART ESP TX=GPIO16, RX=GPIO17. MCP soft-SPI SCK=21 MOSI=22 MISO=23 CS=20 INT=14. SD SPI2 SCK=4 MOSI=5 MISO=6 CS=18.

## Docs

- Master modules: [`master/docs/MASTER.md`](master/docs/MASTER.md)
- Host guide: [`host/HOW_TO_MAKE_A_HOST.md`](host/HOW_TO_MAKE_A_HOST.md)
- Backend contract: `docs/telemetry-api-backend-guide.md`
- Specs under `docs/superpowers/specs/2026-09-04-firmware-v2-milestone*.md`
