# Synthetic UART Transmitter Design

## Goal

Provide a standalone ESP-IDF firmware image for an ESP32-C6 Super Mini that sends realistic synthetic fleet telemetry to a fleet-management device over a TTL UART connection. This is a temporary integration-test image and remains separate from the working ELM327 firmware.

## Hardware interface

- UART peripheral: UART1
- ESP32-C6 TX: GPIO 17 → fleet-device RX
- ESP32-C6 RX: GPIO 16 ← fleet-device TX
- Serial format: 9600 baud, 8 data bits, no parity, 1 stop bit
- ESP32-C6 and fleet device share ground
- Both endpoints must use 3.3 V TTL UART levels; this design does not support RS-232 voltage levels

## Project isolation

The firmware is a separate ESP-IDF project at `examples/synthetic_uart/`. Building or flashing it must not modify or delete the root ELM327 application. The test image temporarily replaces the root firmware only on the connected ESP32-C6 flash.

## Transmitted data

The transmitter sends one newline-delimited JSON object every second. Each object follows the fleet snapshot described in `docs/sample-obd-telemetry.md`.

The object contains:

- Schema version, node ID, and vehicle ID
- Synthetic adapter, profile, and protocol metadata
- Millisecond uptime timestamp and uptime seconds
- Link state and cumulative counters
- RPM, speed, coolant, throttle, and voltage samples

The JSON is compact (no pretty-print whitespace) and terminated by `\n`, allowing the fleet device to use line-oriented framing.

Example:

```json
{"v":1,"node_id":"esp32c6-01","vehicle_id":"fleet-demo-001","ble_peer":"46:FC:0D:32:1E:66","adapter":"MODAXE OBDII","profile":"can_11_500","protocol":"ISO15765-4 CAN11/500","ts_ms":1000,"uptime_s":1,"link":{"ble_connected":true,"elm_ready":true,"poller":"on"},"metrics":{"cmds_ok":1,"cmds_fail":0,"ble_reconnects":0,"blocked_cmds":0,"telemetry_drops":0},"samples":[{"k":"rpm","cmd":"010C","v":780.0,"u":"rpm","raw":"410C0C30","ok":true,"age_ms":0},{"k":"speed","cmd":"010D","v":0,"u":"km/h","raw":"410D00","ok":true,"age_ms":0},{"k":"coolant_c","cmd":"0105","v":87,"u":"C","raw":"41057F","ok":true,"age_ms":0},{"k":"throttle_pct","cmd":"0111","v":14.5,"u":"%","raw":"411125","ok":true,"age_ms":0},{"k":"voltage","cmd":"ATRV","v":13.7,"u":"V","raw":"13.7V","ok":true,"age_ms":0}]}
```

## Synthetic value generation

Values change deterministically on each one-second transmission so integration tests are repeatable:

- RPM: cycles through a realistic idle/driving range
- Speed: ramps up and down between 0 and 80 km/h
- Coolant: remains within 85–95 °C
- Throttle: remains within 10–60%
- Voltage: remains within 13.5–14.2 V

Raw OBD response strings are generated consistently with each decoded value where practical. The transmitter increments `cmds_ok` for each generated snapshot.

## Receive path

UART1 RX is monitored continuously. Any bytes received from the fleet device are copied to the USB Serial/JTAG console with an `[fleet-rx]` prefix. The initial version does not require acknowledgements and does not retransmit missing frames.

## Tasks

Two FreeRTOS tasks keep responsibilities separate:

1. A transmit task generates, serializes, sends, and logs one snapshot per second.
2. A receive task blocks on UART input and logs fleet-device responses.

The transmit task uses cJSON to ensure valid JSON and frees each generated buffer after transmission.

## Error handling

- UART initialization failure is treated as a fatal startup error.
- JSON allocation or serialization failure increments `cmds_fail`, logs an error, skips that frame, and retries one second later.
- Short UART writes are logged and counted as failures.
- RX timeouts are normal and produce no log output.

## Verification

1. Build the standalone project for ESP32-C6.
2. Flash it through the connected USB Serial/JTAG port.
3. Monitor USB logs and confirm one `[fleet-tx]` JSON line per second.
4. Validate each emitted line with a JSON parser.
5. If the fleet device returns bytes, confirm they appear as `[fleet-rx]`.
6. Optionally loop GPIO 17 to GPIO 16 to verify local TX/RX framing before connecting the fleet device.

