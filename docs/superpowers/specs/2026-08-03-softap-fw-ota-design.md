# SoftAP Firmware OTA — Design (Phase 1)

Date: 2026-08-03  
Status: Approved (raw POST + SHA-256 headers)  
Related: `2026-07-31-lte-ota-design.md`, `2026-08-03-ota-backend-api-requirements.md`

## Goal

Prove A/B flash + SHA-256 verify + boot switch + rollback over SoftAP before LTE.

## Component: `fw_ota`

Owns all flash/boot logic. SoftAP (now) and LTE (later) only feed bytes in.

**API (C):**
- `fw_ota_init()` — call early in `app_main`; if pending-verify, run health gate
- `fw_ota_begin(size, sha256_hex)` — lock inactive slot, start hash
- `fw_ota_write(data, len)` — `esp_ota_write` + running SHA-256
- `fw_ota_abort()` — cancel, keep old boot slot
- `fw_ota_end_and_reboot()` — check size + hash → `esp_ota_end` → set boot → reboot
- `fw_ota_get_status(...)` — state, error, running partition, version

**States:** `idle` | `writing` | `failed` | `pending_reboot`  
(After reboot ESP-IDF pending-verify is separate; we mark valid or rollback.)

**Health gate (lab):** mark valid if SoftAP/httpd started successfully. CAN not required.

## SoftAP HTTP

| Method | Path | Behavior |
|--------|------|----------|
| GET | `/api/ota` | JSON status |
| POST | `/api/ota` | Raw `.bin` body |

**POST headers (required):**
- `X-Firmware-Size: <bytes>`
- `X-Firmware-Sha256: <64 hex>`

Reject if busy, size invalid, or hash mismatch after write.

## Out of scope (this phase)

LTE download, ngrok, CM URL, browser multipart UI.

## Lab test (SoftAP web page)

1. Build + flash: `idf.py build flash`
2. Join Wi‑Fi **Fleet-C6**, open `http://192.168.4.1/`
3. **Firmware OTA** card shows booted partition (`ota_0` / `ota_1`), next slot, fw version, state
4. Choose `build/elm327_esp32c6.bin` → **Upload & reboot**
5. After ~10s refresh — booted partition should switch

Optional curl (same API):

```bash
BIN=build/elm327_esp32c6.bin
SHA=$(shasum -a 256 "$BIN" | awk '{print $1}')
SIZE=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN")
curl -v -X POST "http://192.168.4.1/api/ota" \
  -H "Content-Type: application/octet-stream" \
  -H "X-Firmware-Size: $SIZE" \
  -H "X-Firmware-Sha256: $SHA" \
  --data-binary @"$BIN"
```
