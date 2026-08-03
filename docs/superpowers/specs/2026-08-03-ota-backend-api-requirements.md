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

## 4. Publish steps (ops)

1. Take `elm327_esp32c6.bin` from the ESP-IDF build.
2. Compute SHA-256.
3. Host the file on a public HTTPS URL.
4. Serve the manifest JSON above pointing at that URL.
