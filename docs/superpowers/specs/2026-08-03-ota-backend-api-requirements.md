# Fleet OTA — Backend API Requirements (v1)

Device: ESP32-C6 + Quectel EC200U LTE  
Purpose: Tell backend what to expose so the device can download firmware over cellular.

The device needs **two things**:

1. A **manifest** JSON (is there an update, and where is the file?)
2. The **firmware `.bin`** file at the URL in that JSON

Base URL will be configured on the device (CM / lab first, production later). Keep this JSON shape stable.

---

## 1. Manifest API

**Request**

```
GET /firmware/manifest?device_id=<id>&channel=stable
```

| Query | Required | Example |
|-------|----------|---------|
| `device_id` | yes | `fleet-demo-001` |
| `channel` | no (default `stable`) | `stable` |

**Response — `200 OK`**  
`Content-Type: application/json`

```json
{
  "version": "1.2.0",
  "url": "https://cdn.example.com/firmware/1.2.0/elm327_esp32c6.bin",
  "sha256": "e39a3fe9829e11adb2306db59e866d4e7378d275ec99669efc48dffbe56aafa7",
  "size": 1000080,
  "channel": "stable",
  "filename": "elm327_esp32c6.bin",
  "device_id": "fleet-demo-001"
}
```

| Field | Required | Meaning |
|-------|----------|---------|
| `version` | yes | Firmware version string. Must change when the binary changes. |
| `url` | yes | Absolute HTTPS URL of the `.bin` (reachable from the public internet / cellular). |
| `sha256` | yes | Lowercase hex SHA-256 of the `.bin` bytes (64 characters). |
| `size` | yes | Exact byte size of the `.bin`. |
| `channel` | yes | e.g. `stable` |
| `filename` | yes | `elm327_esp32c6.bin` |
| `device_id` | yes | Echo of the requested device id |

**If no update / no firmware:** return `404` with a simple JSON error, e.g. `{"error":"no_firmware"}`.

---

## 2. Binary download

**Request:** `GET` the exact `url` from the manifest.

**Response — `200 OK`**

- Body = raw `elm327_esp32c6.bin` bytes (not zip, not base64, not JSON)
- `Content-Type: application/octet-stream`
- `Content-Length` must match manifest `size`
- File must match manifest `sha256`
- Max size: **1 835 008 bytes** (~1.75 MiB). Current image is ~1.0 MiB.

---

## 3. Hard requirements

- No browser login page or interstitial in front of these endpoints.
- `sha256` and `size` must match the file that is served.

---

## 4. How the device uses this GET API

After LTE is up (~90s), auto-check GETs:

```text
{manifest_url}?device_id=<id>&channel=stable
```

It updates only if manifest `version` differs from **both**:

- the version baked into the running app (`VERSION` at build time), and
- the last successfully applied version in NVS (`ota_applied`)

Then it downloads `url`, verifies `sha256` / `size`, flashes the other OTA slot, and reboots.

Auto-update keeps working as long as: **manifest URL on the device stays valid**, and **each new release has a new `version` string with a matching `.bin`**.

---

## 5. Where to set / change the GET API URL (device)

Set this **once per device** (or whenever the host / ngrok URL changes). Prefer SoftAP or serial so you do not rebuild.

### 5.1 SoftAP (usual)

1. Join SoftAP **`Fleet-C6`** / password **`fleetc6`**.
2. Open the config page.
3. In **LTE OTA → Manifest URL**, set:

   ```text
   https://<your-host>/firmware/manifest
   ```

   Examples:

   - Lab (ngrok): `https://xxxx.ngrok-free.app/firmware/manifest`
   - CM / prod: `https://api.your-backend.com/firmware/manifest`

4. Save (writes NVS).
5. Reboot, or wait for the next auto-check (every 24h after the first).

### 5.2 Serial

```text
ota url https://xxxx.ngrok-free.app/firmware/manifest
ota status
```

Confirm `url:` shows the new value.

### 5.3 Factory default in firmware (optional)

Edit `sdkconfig.defaults`:

```text
CONFIG_FW_OTA_LTE_DEFAULT_MANIFEST_URL="https://…/firmware/manifest"
```

Then rebuild and flash.

**Note:** If NVS already has a URL, **NVS wins**. Clear the SoftAP / NVS URL if you need the Kconfig default to apply.

---

## 6. Checklist — every new firmware version (lab)

Follow this order every release. Skipping “same string in `VERSION` + `release.json` + folder” is what breaks auto-update.

### 6.1 Bump app version

Edit repo root `VERSION` to the new string, e.g.:

```text
1.0.5-lab
```

This string is baked into `elm327_esp32c6.bin` and is what SoftAP shows as “running fw” after a successful install.

### 6.2 Build

```bash
idf.py build
```

Artifact: `build/elm327_esp32c6.bin`

### 6.3 Publish the binary for the lab server

```bash
mkdir -p tools/ota_dev_server/firmware/1.0.5-lab
cp build/elm327_esp32c6.bin \
  tools/ota_dev_server/firmware/1.0.5-lab/elm327_esp32c6.bin
```

Use the same folder name as the version.

### 6.4 Point the lab GET API at that release

Edit **only** `tools/ota_dev_server/release.json`:

```json
{
  "version": "1.0.5-lab",
  "bin": "firmware/1.0.5-lab/elm327_esp32c6.bin",
  "channel": "stable"
}
```

| Field | Must equal |
|-------|------------|
| `release.json` → `version` | New release id (must change every time) |
| `VERSION` used for that build | Same string as `version` |
| `bin` path | The `.bin` copied from that build |

Lab server computes `sha256` / `size` from the file — do not hand-edit them.

### 6.5 Keep the host reachable

```bash
cd tools/ota_dev_server
python3 server.py --host 0.0.0.0 --port 8080
# other terminal:
ngrok http 8080
```

If the **ngrok URL changes**, update the device again (section 5). Path stays `/firmware/manifest`.

### 6.6 Sanity-check the GET response

```bash
curl "https://xxxx.ngrok-free.app/firmware/manifest?device_id=fleet-demo-001&channel=stable"
```

Expect JSON with:

- `"version": "1.0.5-lab"`
- `"url": "https://…/…elm327_esp32c6.bin"`
- `"sha256"` / `"size"` matching the file

### 6.7 Let auto-update run

- Manifest URL already correct in NVS → **reboot** (or wait up to 24h).
- First check ~90s after LTE is ready.
- Expect: download → reboot → SoftAP “running fw” = new version.
- Next check with the same `release.json` → `no_update`.

Manual override if needed: SoftAP **Run** / serial `ota run` (auto always uses `force=false`).

### 6.8 Short cheat sheet

```text
1. Edit VERSION → 1.0.X-lab
2. idf.py build
3. Copy bin → tools/ota_dev_server/firmware/1.0.X-lab/
4. Edit release.json version + bin path (same X)
5. Ensure server + ngrok running; curl the GET once
6. Device URL still correct? If ngrok changed → SoftAP/serial update URL
7. Reboot device → wait for LTE auto-check
```

---

## 7. Publish steps — CM / production backend

Firmware stays the same. Only the hosted GET + binary change.

1. Take `elm327_esp32c6.bin` from the ESP-IDF build (built with the new `VERSION`).
2. Compute SHA-256 and byte size of that file.
3. Host the file on a public HTTPS URL (no login interstitial).
4. Serve the manifest JSON (section 1) with:
   - new `version` (same as build `VERSION`)
   - `url` pointing at that hosted `.bin`
   - matching `sha256` and `size`
5. On each device (or SoftAP fleet config), set manifest URL to production  
   `https://…/firmware/manifest` if not already set (section 5).

You do **not** change SoftAP UI or OTA C code for each release.

---

## 8. Do / don’t

| Do | Don’t |
|----|--------|
| Change `VERSION` + rebuild for every real new firmware | Bump only manifest / `release.json` `version` while serving an old `.bin` |
| Put that same string in the GET response `version` | Reuse the same `version` string for a different binary |
| Keep `url` / lab `bin` pointing at that new file | Leave a sticky bad URL (e.g. lab `?scenario=gone`) in NVS |
| Update device URL when ngrok / host changes | Expect SoftAP “running fw” to change without a successful OTA of the new bin |
