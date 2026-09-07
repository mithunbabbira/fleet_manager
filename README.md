# ESP32-C6 Fleet Telematics Node

ESP-IDF firmware that reads OBD-II through an **MCP2515 CAN controller**,
exposes configuration over **USB serial** (plus an optional PC **Carrier Console**),
and uploads telemetry through a **Quectel EC200U LTE modem**. Optional Zigbee
sensor hosts join the carrier as dynamic endpoints.

**Product firmware lives in [`firmware_v2/master/`](firmware_v2/master/)** (CMake
project `fleet_v2_master`). There is no root ESP-IDF app in this repository.

Pin map and printed-PCB wiring:
[`hardware/fleet_telematics_carrier/README.md`](hardware/fleet_telematics_carrier/README.md).

Design notes live under [`docs/superpowers/`](docs/superpowers/). Early BLE/ELM327
docs are historical only ([`docs/archive/`](docs/archive/)).

## Hardware requirements

Use the **printed carrier PCB** (`Vehical_Telematics_Design.zip`). Do not
recreate the old jumper harness or fab the retired Python Gerber draft.

| Item | Notes |
|---|---|
| **ESP32-C6 Super Mini** | **4 MB flash**. Dual-bank OTA: `ota_0` + `ota_1`. |
| **MCP2515 + TXS0108E** | Soft-SPI GPIO21 SCK, 22 MOSI, 23 MISO, 20 CS, 14 INT (3.3 V ↔ 5 V). |
| **microSD** | Dedicated SPI2: GPIO4 SCK, 5 MOSI, 6 MISO, 18 CS. |
| **Quectel EC200U** | UART1 **GPIO16 TX → modem RX**, **GPIO17 RX ← modem TX**, 115200 8N1. Separate VBAT + common GND. |

## Build and flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v5.2.3** with the ESP32-C6 tool chain.

```bash
source ~/esp/esp-idf/export.sh   # must report ESP-IDF v5.2.3
cd firmware_v2/master
idf.py set-target esp32c6
idf.py build
# Prefer Trafyn OTA for field updates. USB flash for recovery:
# idf.py -p PORT flash monitor
```

Artifact: `firmware_v2/master/build/fleet_v2_master.bin`.

Version string: [`firmware_v2/master/VERSION`](firmware_v2/master/VERSION).

Module map: [`firmware_v2/master/docs/MASTER.md`](firmware_v2/master/docs/MASTER.md).

## Carrier Console (PC web UI)

Configure the carrier from your computer over USB:

```bash
python3 -m pip install -r tools/carrier_console/requirements.txt
python3 tools/carrier_console/app.py --port /dev/cu.usbmodem1101
```

Opens **http://127.0.0.1:8766**. See [`tools/carrier_console/README.md`](tools/carrier_console/README.md).

## Lab telemetry mock

```bash
python3 tools/telemetry_mock/server.py
```

Matches Trafyn `POST /nc-events-api/v1/sources/nc-fleet-device/messages`.
See [`tools/telemetry_mock/README.md`](tools/telemetry_mock/README.md).

## Host unit tests

```bash
cmake -S tests/host -B tests/host/build
cmake --build tests/host/build
ctest --test-dir tests/host/build --output-on-failure
```

## Zigbee hosts

Host HOWTO: [`firmware_v2/host/HOW_TO_MAKE_A_HOST.md`](firmware_v2/host/HOW_TO_MAKE_A_HOST.md).

Hardware host projects live under
[`hardware/fleet_telematics_carrier/host/`](hardware/fleet_telematics_carrier/host/).

## CI

Bitbucket Pipelines ([`bitbucket-pipelines.yml`](bitbucket-pipelines.yml)) builds
`firmware_v2/master` via [`tools/ci/build_firmware.sh`](tools/ci/build_firmware.sh)
and runs the host test suite before packaging.

## Docs

- Backend uplink contract: [`docs/telemetry-api-backend-guide.md`](docs/telemetry-api-backend-guide.md)
- Trafyn OTA: [`docs/superpowers/specs/2026-08-19-trafyn-firmware-ota-design.md`](docs/superpowers/specs/2026-08-19-trafyn-firmware-ota-design.md)
- Firmware v2 overview: [`firmware_v2/README.md`](firmware_v2/README.md)
