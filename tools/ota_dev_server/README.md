# OTA lab server (manifest + `.bin` hosting)

Stdlib-only Python HTTP server for dual-bank OTA bring-up. **Not** linked into the ESP-IDF firmware build.

## Run

```bash
cd tools/ota_dev_server
python3 server.py --host 0.0.0.0 --port 8080
```

## Publish a build

```bash
# after idf.py build at repo root
VER=1.0.0
mkdir -p firmware/$VER
cp ../../build/elm327_esp32c6.bin firmware/$VER/
shasum -a 256 firmware/$VER/elm327_esp32c6.bin | awk '{print $1}' > firmware/$VER/elm327_esp32c6.bin.sha256
```

## API

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/health` | Liveness + list of published versions |
| GET | `/firmware/manifest?device_id=…&channel=stable` | Latest (or `?version=`) manifest JSON |
| GET | `/firmware/files/<version>/elm327_esp32c6.bin` | Binary download |

Example manifest:

```json
{
  "version": "1.0.0",
  "url": "http://192.168.1.10:8080/firmware/files/1.0.0/elm327_esp32c6.bin",
  "sha256": "…",
  "size": 1000080,
  "channel": "stable",
  "filename": "elm327_esp32c6.bin",
  "device_id": "fleet-demo-001"
}
```

Point the device (SoftAP LAN or LTE tunnel) at this host when implementing the OTA client. Production should use HTTPS + CDN; this folder is for lab only.
