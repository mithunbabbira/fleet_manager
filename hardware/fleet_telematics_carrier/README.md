# Fleet telematics carrier (printed PCB)

This is the **authoritative hardware map** for the fabricated board. Firmware
defaults match this copper. Do not respin the PCB for LTE RX/TX — UART pins
were aligned in software on 2026-08-18.

Fabricated gerbers (KiCad 9.0.9, 2026-08-12): repo root
`Vehical_Telematics_Design.zip`. Close-up photos of the assembled carrier are
in `wiring_diagrams/`.

A Python Gerber generator used to live here. It was a **pre-fab draft** and
does **not** match the printed LTE UART nets. It has been removed.

## Modules on the board

| Module | Role |
|---|---|
| ESP32-C6 Super Mini | MCU, USB Serial/JTAG console |
| TXS0108E | 3.3 V (A) ↔ 5 V (B) for MCP2515 SPI + INT |
| MCP2515 + TJA1050 | Vehicle CAN |
| microSD reader | Durable uplink queue |
| Quectel EC200U | LTE UART + GNSS |
| Separate LTE VBAT | Modem supply, common GND with ESP |
| Optional ESP32-C6 host | Zigbee end device (e.g. UL212 BLE fetch) — not on carrier PCB |

## Optional Zigbee sensor host

A second ESP32-C6 can sit beside the carrier and join the coordinator over Zigbee
(open network, channel 15). Reference firmware:
`host/ul212-ble-fetch/`. Docs: `docs/fleet-zigbee-host-guide.md`.

## Validated firmware nets (2026-08-18)

MCP, SD, SoftAP, and LTE AT were proven on this copper. LTE UART uses the
**printed** mapping (same as the old jumper that already worked), not a
textbook TX↔RX cross.

### ESP32-C6 GPIO

| Function | GPIO | Notes |
|---|---:|---|
| MCP2515 SCK | 21 | Soft-SPI (bit-bang) |
| MCP2515 MOSI (SI) | 22 | Soft-SPI |
| MCP2515 MISO (SO) | 23 | Soft-SPI |
| MCP2515 CS | 20 | Idle high at boot |
| MCP2515 INT | 14 | Reserved; driver still polls |
| microSD SCK | 4 | Hardware SPI2 |
| microSD MOSI | 5 | Hardware SPI2 |
| microSD MISO | 6 | Hardware SPI2 |
| microSD CS | 18 | Idle high at boot |
| EC200U UART — ESP TX | **16** | ESP **GPIO16 → modem RX** |
| EC200U UART — ESP RX | **17** | ESP **GPIO17 ← modem TX** |

UART1, 115200 8N1. Kconfig: `CONFIG_NET_LTE_UART_TX_GPIO=16`,
`CONFIG_NET_LTE_UART_RX_GPIO=17`.

KiCad net names on the printed board are `/GMS_TX` and `/GMS_RX` for the modem
UART, and `/A1`–`/A5` / `/B1`–`/B5` for the TXS channels.

### TXS0108E ↔ MCP2515 (working orientation)

A-side = ESP 3.3 V, B-side = MCP 5 V, `OE` = 3.3 V.

| MCP2515 | TXS B | TXS A | ESP GPIO |
|---|---|---|---:|
| CS | B1 | A1 | 20 |
| SO (MISO) | B2 | A2 | 23 |
| SI (MOSI) | B3 | A3 | 22 |
| SCK | B4 | A4 | 21 |
| INT | B5 | A5 | 14 |

TXS breakout silkscreen can read A8→A1 left-to-right. Match **net names**
`/A1`…`/A5`, not assumed left-to-right pin order.

### LTE UART (printed copper)

| ESP pin | Modem pin (module silk TX/RX) |
|---|---|
| GPIO16 | RX (modem receive) |
| GPIO17 | TX (modem transmit) |
| GND | GND |

This looks “same-name inverted” versus a textbook UART cross. It is what the
printed board and the old working jumper both use. Firmware follows the copper.

Do **not** flash an older image that still sets TX=GPIO17 / RX=GPIO16.

Modem needs its own VBAT (~3.7–4.2 V, peak current several hundred mA to ~2 A),
PWRKEY to boot, and **NETLIGHT** activity. Unplug the EC200U USB cable while
the ESP owns the UART (many breakouts share one UART with USB-serial).

## What is used vs not used

**Used (production firmware + this PCB):**

- MCP2515 **soft-SPI** on 21/22/23/20/14 through TXS0108E
- microSD **dedicated SPI2** on 4/5/6/18 (not shared with MCP)
- EC200U on UART1 **GPIO16 TX / GPIO17 RX**
- USB Serial/JTAG console; SoftAP `Fleet-C6` / `fleetc61` → `http://192.168.4.1/`
- Dual-bank OTA (`ota_0` + `ota_1`)

**Not used (do not wire or revive):**

- Shared SPI between MCP and SD (old smoke-test idea)
- Textbook LTE map ESP GPIO17=TX / GPIO16=RX
- BLE ELM327 adapter path (deleted from firmware)
- Python `generate_gerbers.py` draft carrier (removed; not the fabricated board)

## Lab proof (2026-08-18)

On the printed carrier with the UART firmware invert:

- `mcp2515: detected (CANSTAT=0x80)`
- `store_sd` mounted (SD32G)
- `lte: uart_ok=yes sim=yes reg=yes attached=yes csq=18(-77dBm) op="IND airtel"`
- HTTP left the modem (server returned 400 — API issue, not UART)

CAN `TX not acked` is expected with no vehicle on the bus.
