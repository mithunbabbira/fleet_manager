# SD SPI smoke test (ESP32-C6)

Validates the **printed carrier** microSD path: dedicated SPI2, not shared
with MCP2515.

## Wiring (production PCB)

| Signal | GPIO |
|--------|------|
| SCK | 4 |
| MOSI | 5 |
| MISO | 6 |
| SD CS | 18 |
| MCP2515 CS | 20 (held HIGH during this test) |

## Run

```bash
cd examples/sd_smoke
source ~/esp/esp-idf/export.sh
idf.py set-target esp32c6
idf.py -p /dev/cu.usbmodemXXXX build flash monitor
```

Look for `SD SMOKE: PASS`.

If the card is not FAT32, the test retries once with `format_if_mount_failed` (erases the card).

Restore the main fleet app from the repo root with `idf.py -p PORT flash`.
