# OTA lab server

Simple stand-in for the backend GET API.

## Config — edit this only

`tools/ota_dev_server/release.json`:

```json
{
  "version": "1.0.4-lab",
  "bin": "firmware/1.0.4-lab/elm327_esp32c6.bin",
  "channel": "stable"
}
```

- **`version`** → GET `/firmware/manifest` (device compares this).
- **`bin`** → file to download (`sha256`/`size` computed automatically).

## Run

```bash
cd tools/ota_dev_server
python3 server.py --host 0.0.0.0 --port 8080
# ngrok http 8080
```

## API

| GET | Purpose |
|-----|---------|
| `/firmware/manifest?device_id=…&channel=stable` | JSON: version, url, sha256, size, … |
| `/firmware/active/elm327_esp32c6.bin` | Active binary from `release.json` |

## Lab test

1. Set `version` different from the device → reboot / wait for auto-check → downloads once.
2. Same version → `no_update`.
