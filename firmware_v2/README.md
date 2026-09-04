# Firmware v2

Clean rewrite of the fleet telematics stack. The legacy tree (`components/`, root `main/`, old hosts) stays as **reference** until we delete it later.

## Layout

| Path | Role |
|------|------|
| [`master/`](master/) | ESP-IDF app for the soldered carrier PCB (ESP32-C6) |
| [`host/`](host/) | Rules for building Zigbee sensor hosts that join the master |

## Milestone roadmap

1. **M1 (done):** master boot, dual-bank OTA, LTE HTTP GET/POST, Trafyn get-latest OTA, host HOWTO only  
2. **M2 (next):** GPS + time — see `docs/superpowers/specs/2026-09-04-firmware-v2-milestone2-design.md`  
3. **M3:** SD store-on-fail queue  
4. **M4:** OBD / MCP2515  
5. **M5:** Zigbee coordinator + dynamic hosts + JSON 1087 / 1088 / 1089 uplink  

JSON envelope (unchanged plan): top-level `device_id`, `node_id`, `schemaId`, `ts_ms`, `payload`.

## Build master (M1)

```bash
cd firmware_v2/master
source ~/esp/esp-idf/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

**Pins:** LTE UART ESP TX=GPIO16, RX=GPIO17 (do not change — PCB is soldered).

## Docs

- Design: `docs/superpowers/specs/2026-09-04-firmware-v2-milestone1-design.md`
- Master modules: [`master/docs/MASTER.md`](master/docs/MASTER.md)
- Host guide: [`host/HOW_TO_MAKE_A_HOST.md`](host/HOW_TO_MAKE_A_HOST.md)
