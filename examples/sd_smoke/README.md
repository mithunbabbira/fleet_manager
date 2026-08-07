# SD SPI smoke test (ESP32-C6)

Validates the soldered microSD module on the shared SPI bus with MCP2515.

## Wiring assumed

| Signal | GPIO |
|--------|------|
| SCK | 21 |
| MOSI | 22 |
| MISO | 23 |
| SD CS | 18 |
| MCP2515 CS | 20 (held HIGH during this test) |

Change `PIN_SD_CS` in `main/app_main.c` if your CS is different.

## Run

```bash
cd examples/sd_smoke
source ~/esp/esp-idf/export.sh
idf.py set-target esp32c6
idf.py -p /dev/cu.usbmodemXXXX build flash monitor
```

Look for `SD SMOKE: PASS`.

If the card is not FAT32, the test retries once with `format_if_mount_failed` (erases the card).
