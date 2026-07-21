# Synthetic UART Fleet Transmitter

Standalone ESP32-C6 test firmware. It sends one compact fleet telemetry JSON
object per second over UART1.

## Wiring

| ESP32-C6 Super Mini | Fleet device |
|---|---|
| GPIO 17 (TX) | RX |
| GPIO 16 (RX) | TX |
| GND | GND |

Both endpoints must use 3.3 V TTL UART levels. Do not connect directly to an
RS-232 voltage interface.

Serial format: 9600 baud, 8 data bits, no parity, 1 stop bit.

## Build and flash

```bash
source ~/esp/esp-idf/export.sh
idf.py -C examples/synthetic_uart set-target esp32c6
idf.py -C examples/synthetic_uart build
idf.py -C examples/synthetic_uart -p /dev/cu.usbmodemXXXX flash monitor
```

Exit the monitor with `Ctrl+]`.

## Framing

Each UART frame is one compact JSON object followed by LF (`\n`). The fleet
device should buffer bytes until LF, then parse the complete line as JSON.

USB console output uses:

```text
[fleet-tx] {"v":1,...}
[fleet-rx] ACK
```

The receiver does not require ACKs and does not retransmit frames.
