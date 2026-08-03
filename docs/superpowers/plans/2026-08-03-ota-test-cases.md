# OTA lab cases (SoftAP + LTE) — checklist

Device compares `manifest.version` to **build** `VERSION` / `esp_app_desc.version` (not NVS).  
When CM/prod API is ready: change SoftAP **manifest URL** only — same JSON contract.

## Prep

```bash
# VERSION file drives both firmware string and publish folder name
idf.py build
./tools/ota_dev_server/publish.sh
# restart lab server if needed; keep ngrok up
```

Manifest base (swap later for backend):
`https://<ngrok-or-cm>/firmware/manifest`

---

## Cases

| ID | Case | How | Pass |
|----|------|-----|------|
| **T1** | SoftAP A↔B | SoftAP **Firmware OTA** upload `.bin` | Booted partition flips; confirmed |
| **T2** | `no_update` | Build+publish same `VERSION`; Force **off**; Run LTE OTA | phase `no_update`; no reboot |
| **T3** | Happy LTE update | Bump `VERSION`, build, publish; Force off; Run LTE OTA | download → other slot → confirm |
| **T4** | Bad SHA-256 | Manifest URL `.../manifest?scenario=bad_sha` | phase `failed` (sha256_mismatch); slot unchanged |
| **T5** | Truncated bin | URL `.../manifest?scenario=truncate` | `failed` (size/stream); slot unchanged |
| **T6** | Missing firmware | URL `.../manifest?scenario=gone` | `failed` / HTTP 404; no flash |
| *(after T4–T6)* | Reset URL | SoftAP **Clear lab scenario from URL**, or serial `ota url <base>` / `ota test t2` | URL must **not** keep `?scenario=…` or every Run stays 404 / faulted |
| **T7** | LTE down | Pull SIM / bad APN; Run LTE OTA | PDP/UART error; no flash |
| **T8** | Concurrent | Start LTE run, quickly SoftAP upload | SoftAP **409 busy** (or LTE busy) |
| **T9** | Power loss mid-download | Unplug during download | Still boots previous slot |
| **T10** | Rollback | SoftAP flash intentionally broken image (advanced) | Bootloader rolls back if not confirmed |

## Lab fault injection (server)

Append to manifest URL (device already adds `device_id` & `channel`):

- `?scenario=bad_sha`
- `?scenario=truncate`
- `?scenario=gone`

Example saved URL:
`https://xxxx.ngrok-free.app/firmware/manifest?scenario=bad_sha`

## Backend swap

| Now | Later |
|-----|--------|
| ngrok → `tools/ota_dev_server` | CM/prod HTTPS manifest |
| Same fields: `version`, `url`, `sha256`, `size`, `channel`, `filename` | unchanged |

No firmware change required beyond NVS/SoftAP **manifest_url**.
