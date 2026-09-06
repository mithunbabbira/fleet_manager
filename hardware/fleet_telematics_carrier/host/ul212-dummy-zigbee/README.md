# UL212 dummy Zigbee host (ESP32-C6 Super Mini)

Synthetic fuel readings over Zigbee for car/lab tests **without** a UL212 sensor.

Must match the carrier:

- Channel **15**
- EPAN **`F1EE700000000001`**
- Schema **1088** (`ul212-dummy-001`)

## Build / flash

```bash
cd hardware/fleet_telematics_carrier/host/ul212-dummy-zigbee
pio run -e esp32-c6-devkitc-1
pio run -e esp32-c6-devkitc-1 -t upload -t monitor
```

If upload fails on Super Mini: hold **BOOT**, tap **RST**, retry upload. Optional XIAO env: `-e seeed_xiao_esp32c6`.

## Verify

1. Carrier on same EPAN/channel (firmware_v2 master).
2. Dummy USB log: `[zb] start …` then join / `[zb] report seq=… height=…`
3. Carrier CLI: `fleet hosts` → `ul212-dummy-001` `link=1`, changing `height_mm`
4. Optional: uplink tick / ngrok → schema **1088**

## Config

Edit `include/zigbee_app_config.h` then rebuild (IDs / EPAN / channel).
