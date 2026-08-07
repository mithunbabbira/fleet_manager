# Fleet Telematics Carrier PCB

This directory contains a draft Gerber package for a module-carrier PCB based on
the hardware documented in this repository.

## Scope

The design is a carrier/interconnect board for these already-used modules:

- ESP32-C6 Super Mini, wired through a labeled 2.54 mm header
- TXS0108E 8-channel bidirectional level converter between ESP32-C6 and MCP2515
- MCP2515 + TJA1050 CAN module
- Quectel EC200U LTE module or breakout
- XY-3606 buck converter module
- OBD cable/terminal input and separate LTE power input

It is not a bare-chip EC200U or bare MCP2515 layout. The repo does not contain
manufacturer footprints, complete module dimensions, surge/ESD protection, or a
reviewed production schematic, so the generated files should be treated as a
reviewable fabrication draft, not a release-to-manufacturing package.

## Validated Firmware Nets

| Function | ESP32-C6 GPIO |
|---|---:|
| MCP2515 SCK (soft-SPI) | GPIO21 |
| MCP2515 MOSI | GPIO22 |
| MCP2515 MISO | GPIO23 |
| MCP2515 CS | GPIO20 |
| MCP2515 INT | GPIO14 |
| microSD SCK (hardware SPI2) | GPIO4 |
| microSD MOSI | GPIO5 |
| microSD MISO | GPIO6 |
| microSD CS | GPIO18 |
| EC200U RXD, ESP -> modem | GPIO17 |
| EC200U TXD, ESP <- modem | GPIO16 |

The TXS0108E A side is tied to ESP32-C6 3V3 logic, and the B side is tied to the
MCP2515 module's 5V logic. `OE` is tied to 3V3.

### SPI layout (separate wires)

ESP32-C6 has **one** general-purpose SPI controller (SPI2). Production wiring uses
**separate pins** for CAN and SD:

| Device | How | Pins |
|---|---|---|
| microSD | Hardware SPI2 | SCK4 MOSI5 MISO6 CS18 |
| MCP2515 | Soft-SPI (GPIO) | SCK21 MOSI22 MISO23 CS20 |

No shared SCK/MOSI/MISO. Optional: 10 kΩ pull-ups to 3.3 V on both CS lines.

## Generated Outputs

Run:

```bash
python3 hardware/fleet_telematics_carrier/generate_gerbers.py
```

The script writes:

- `gerbers/fleet_telematics_carrier-F_Cu.gbr`
- `gerbers/fleet_telematics_carrier-B_Cu.gbr`
- `gerbers/fleet_telematics_carrier-F_Mask.gbr`
- `gerbers/fleet_telematics_carrier-B_Mask.gbr`
- `gerbers/fleet_telematics_carrier-F_SilkS.gbr`
- `gerbers/fleet_telematics_carrier-Edge_Cuts.gbr`
- `gerbers/fleet_telematics_carrier-PTH.drl`
- `gerbers/fleet_telematics_carrier-NPTH.drl`
- `gerbers/bom.csv`
- `gerbers/netlist.csv`
- `fleet_telematics_carrier_gerbers.zip`

## Pre-Fab Checks

Before ordering boards, verify:

- Exact pin order of the ESP32-C6 Super Mini board you will solder or wire.
- Exact pin order of the TXS0108E breakout and MCP2515+TJA1050 module.
- Whether your MCP2515 module already has CAN termination installed.
- Whether the selected XY-3606 setting and copper width are suitable for the
  real load.
- EC200U peak-current power path, PWRKEY timing, antenna/SIM clearances, and
  common-ground strategy.
- Vehicle transient, reverse-polarity, fuse, ESD, and enclosure clearances.

