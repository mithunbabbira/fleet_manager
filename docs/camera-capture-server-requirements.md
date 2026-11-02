# Cabin camera — server requirements

**Audience:** Backend (Spring Boot)  
**Status:** Contract for implementation. Firmware lab proved the path on one ESP32-C6 + EC200U. Production firmware is not ported yet.  
**Identity:** Use the existing provisioned `device_id` (same string as telemetry / OTA). Do not introduce a second id. Modem IMEI is not the routing key.

---

## 1. What the server owns

The server tells **one** device to take a still photo, then stores the JPEG that device uploads.

```text
Operator / fleet API
    POST /v1/camera/jobs  { device_id, deg }
        │
        ▼
Spring Boot
    publish MQTT  fleet/v1/{device_id}/camera/cmd
        │  internet (broker)
        ▼
EC200U on that truck only  (subscribed to its own topic)
        │ UART
        ▼
ESP32-C6  → SoftAP → ESP32-CAM (servo, then still JPEG)
        │ LTE HTTPS multipart
        ▼
Spring Boot  POST /v1/camera/uploads
    stores object, marks job uploaded
        │
        ▼
Operator  GET /v1/camera/jobs/{job_id}
          GET /v1/camera/jobs/{job_id}/image
```

The device does **not** poll. The server pushes the command. The image does **not** travel on MQTT.

---

## 2. Out of scope for this API

- OBD / GPS telemetry (`nc-events-api`, schema 1087 / 1088 / 1089). Do not put the JPEG in that pipeline.
- Firmware OTA.
- Live video / MJPEG.
- Broadcasting one command to every truck.
- The ESP32 SoftAP between master and camera (device-local only).

---

## 3. Identity and tenancy

| Field | Rule |
|-------|------|
| `device_id` | Required. Same value already stored on the device (NVS) and sent on telemetry. Max 40 chars. Charset: `A–Z a–z 0–9 . _ -`. |
| `job_id` | Server-generated. Unique. Opaque. Max 64 chars. Same charset. The device echoes it. It is not a filename the client chooses freely beyond this pattern. |
| Who may be triggered | Only a `device_id` that exists in the fleet registry and is allowed camera capture. Unknown id → `404`. |

One in-flight camera job per `device_id`. A second request while the first is not terminal returns `409` with the active `job_id`.

---

## 4. Job state machine

```text
accepted → published → started → uploaded
                 │         │
                 ▼         ▼
               failed    failed
                 │
                 ▼
              expired   (no terminal ack/upload before deadline)
```

| State | Meaning |
|-------|---------|
| `accepted` | Row created. MQTT not sent yet. |
| `published` | Broker accepted the command (QoS 1 publish complete). |
| `started` | Device ACK `started` (it began capture). |
| `uploaded` | JPEG stored and linked to this `job_id`. Terminal success. |
| `failed` | Device ACK `fail`, or upload rejected. Terminal. |
| `expired` | No `uploaded` and no terminal `fail` within **120 seconds** of `published`. Terminal. |

Idempotency:

- Creating a job is not idempotent. Each call is a new `job_id`.
- Upload with the same `job_id` after `uploaded` returns `200` and does **not** store a second object.
- Upload with an unknown or expired `job_id` returns `409` or `410`. Do not create a job from an upload alone.
- Device ACK for a `job_id` already terminal is ignored.
- A stored JPEG is the success signal. Commit `uploaded` before the HTTP response is sent. A later ACK `fail` / `upload` must leave the job `uploaded`. The lab firmware currently sends that false `fail` after HTTP 200 because it does not parse the modem `+QHTTPPOST` result.

Suggested columns: `job_id`, `device_id`, `deg`, `state`, `fail_reason`, `created_at`, `published_at`, `started_at`, `uploaded_at`, `object_key`, `byte_size`, `content_type`.

---

## 5. MQTT (command path)

Broker is **yours** (not the public `test.mosquitto.org` used in the lab). TLS. Username/password or client certs.

ACL:

- Each device may **subscribe** only to `fleet/v1/{its device_id}/camera/cmd`.
- Each device may **publish** only to `fleet/v1/{its device_id}/camera/ack`.
- The server publishes commands and subscribes to `fleet/v1/+/camera/ack`.
- No shared topic that every device listens to.

### 5.1 Command (server → device)

| | |
|--|--|
| Topic | `fleet/v1/{device_id}/camera/cmd` |
| QoS | 1 |
| Retain | false |

Payload (UTF-8 JSON, under 256 bytes):

```json
{
  "job_id": "cam-01HXYZ",
  "deg": 90
}
```

| Field | Rule |
|-------|------|
| `job_id` | Required. Server-generated. |
| `deg` | Required integer. Servo angle **0–180**. Device clamps, but the API rejects values outside that range with `400`. |

Do not put image bytes, URLs with secrets, or a list of devices in this message. The device is configured with the upload URL out of band. The server does not send that URL in the command.

### 5.2 Ack (device → server)

| | |
|--|--|
| Topic | `fleet/v1/{device_id}/camera/ack` |
| QoS | 1 |

```json
{
  "job_id": "cam-01HXYZ",
  "device_id": "fleet-demo-001",
  "status": "started",
  "detail": "capture"
}
```

| `status` | `detail` examples | Server action |
|----------|-------------------|---------------|
| `started` | `capture` | `published` or `accepted` → `started` |
| `uploaded` | `ok` | If JPEG already stored, stay `uploaded`. If not, wait for HTTP (ack may arrive first or second). |
| `fail` | `capture`, `upload`, `busy` | → `failed` only if no JPEG is stored yet. Store `detail` as `fail_reason`. If state is already `uploaded`, ignore this ack. `busy` means the device ignored this job because another capture is running. |

`device_id` in the payload must match the topic. Mismatch → drop and log.

The server must keep an MQTT consumer running. Ack handling is idempotent.

---

## 6. HTTP API

Base path suggestion: `/v1/camera`. JSON unless noted. Timestamps are UTC ISO-8601.

Auth for operator routes: your existing fleet API auth (same as other internal APIs).  
Auth for the device upload route: see §7. Do not leave it anonymous in production.

### 6.1 Create job (operator)

`POST /v1/camera/jobs`

```json
{
  "device_id": "fleet-demo-001",
  "deg": 90
}
```

`201 Created`

```json
{
  "job_id": "cam-01HXYZ",
  "device_id": "fleet-demo-001",
  "deg": 90,
  "state": "published",
  "created_at": "2026-10-21T10:00:00Z"
}
```

Errors:

| HTTP | When |
|------|------|
| 400 | Missing field, `deg` not in 0..180, bad `device_id` charset |
| 404 | `device_id` not in fleet |
| 409 | That device already has a non-terminal camera job. Body includes `active_job_id` |
| 502 | MQTT publish failed. Job stored as `failed` with `fail_reason=publish` |

The handler publishes MQTT before returning `201`. If publish fails, do not return success.

### 6.2 Get job (operator)

`GET /v1/camera/jobs/{job_id}`

`200`

```json
{
  "job_id": "cam-01HXYZ",
  "device_id": "fleet-demo-001",
  "deg": 90,
  "state": "uploaded",
  "fail_reason": null,
  "byte_size": 52000,
  "created_at": "2026-10-21T10:00:00Z",
  "published_at": "2026-10-21T10:00:00Z",
  "started_at": "2026-10-21T10:00:02Z",
  "uploaded_at": "2026-10-21T10:00:06Z",
  "image_url": "/v1/camera/jobs/cam-01HXYZ/image"
}
```

`404` if unknown.

### 6.3 List jobs (operator)

`GET /v1/camera/jobs?device_id={device_id}&limit=20`

Newest first. `device_id` optional. `limit` default 20, max 100.

### 6.4 Device upload (device → server)

`POST /v1/camera/uploads`  
`Content-Type: multipart/form-data`

| Part | Required | Notes |
|------|----------|-------|
| `job_id` | yes | Text. Must match an open job. |
| `device_id` | yes | Text. Must match the job’s device. |
| `deg` | no | Text integer. Stored if present. |
| `file` | yes | JPEG. `Content-Type: image/jpeg`. Filename ignored. |

Validation:

- Body size limit **2 MB** (lab JPEGs are about 30–80 KB).
- File must start with JPEG SOI `FF D8`. Reject otherwise (`400`).
- `job_id` + `device_id` must match the job row.
- Job state must be `published` or `started`. 
- If state is already `uploaded`, return `200` with the existing object (idempotent).
- If state is `failed` or `expired`, return `409`.

`200`

```json
{
  "ok": true,
  "job_id": "cam-01HXYZ",
  "device_id": "fleet-demo-001",
  "bytes": 52000,
  "state": "uploaded"
}
```

On success: write the object, set `byte_size`, `uploaded_at`, state `uploaded`, then send the HTTP response. The device may publish ACK `fail` / `upload` after it receives that response. That ack must not undo `uploaded`.

Storage key suggestion: `camera/{device_id}/{job_id}.jpg`. Overwrite only for the same `job_id` retry before first success. Do not use the client filename.

The device speaks **Quectel QHTTP** (HTTPS POST). It does not use chunked encoding reliably. Accept a normal `Content-Length` body. TLS certificate must be a public CA (the modem validates with a relaxed lab setting today; production should use a normal public cert, not a private CA).

### 6.5 Get image (operator)

`GET /v1/camera/jobs/{job_id}/image`

`200` with `Content-Type: image/jpeg` and the bytes.  
`404` if the job does not exist.  
`409` if the job exists but state is not `uploaded`.

A short-lived pre-signed URL is acceptable later. v1 can stream the bytes.

---

## 7. Device authentication on upload

v1 minimum:

- Header `Authorization: Bearer <device-token>` 
- Token is provisioned per `device_id` (same operational step as today’s device provisioning).
- Reject `401` if missing or wrong.
- Token in the header, not in the MQTT payload and not in the JPEG.
- The lab device does not send this header yet. A production server that requires it will return `401` for that board until firmware adds the header. Auth acceptance tests need their own client.

Operator routes use the existing user/service auth, not the device token.

MQTT: per-device credentials and the ACL in §5. The device must not be able to publish to another device’s command topic.

---

## 8. Timeouts and concurrency

| Item | Value |
|------|-------|
| Servo + capture on device | typically 1–3 s after the command (longer if the angle change is large) |
| Job expiry | 120 s after `published_at` |
| Upload read timeout | 120 s (modem `AT+QHTTPPOST` waits 120 s for the response) |
| Max concurrent uploads | bound by server; device sends one JPEG at a time |
| In-flight jobs per device | 1 |

A scheduler (or delayed message) moves `published` / `started` jobs to `expired` at 120 s if they are still open. Expiry must not delete a JPEG that arrived in the same moment: if the object write commits, state stays `uploaded`.

---

## 9. Spring Boot shape (suggested, not mandatory)

| Component | Responsibility |
|-----------|----------------|
| `CameraJobController` | `POST/GET /v1/camera/jobs`, `GET .../image` |
| `CameraUploadController` | `POST /v1/camera/uploads` (multipart) |
| `CameraJobService` | State transitions, one-in-flight rule, expiry |
| `CameraCommandPublisher` | MQTT publish QoS 1, topic from `device_id` |
| `CameraAckListener` | MQTT subscribe `fleet/v1/+/camera/ack`, update state |
| `CameraImageStore` | S3 or equivalent. DB holds the key, not the bytes |
| `camera_job` table | §4 columns. Unique `job_id`. Index `(device_id, state)` |

MQTT client: one shared connection for the application, not one connection per request. Reconnect with backoff. Publish failure fails the HTTP create call.

Do not run the broker inside this service. Use the platform MQTT broker.

---

## 10. Example sequence

1. `POST /v1/camera/jobs` `{ "device_id": "fleet-demo-001", "deg": 90 }`
2. Server inserts job `cam-01HXYZ`, publishes  
   `fleet/v1/fleet-demo-001/camera/cmd` → `{"job_id":"cam-01HXYZ","deg":90}`
3. Returns `201` state `published`.
4. Device ACK `started`.
5. Device `POST /v1/camera/uploads` with `job_id`, `device_id`, JPEG file.
6. Server stores `camera/fleet-demo-001/cam-01HXYZ.jpg`, state `uploaded`.
7. Device ACK `uploaded` (optional if HTTP already succeeded; both must be safe).
8. Operator `GET /v1/camera/jobs/cam-01HXYZ/image`.

Failure example: camera offline. Device ACK `fail` / `capture`. State `failed`. No object. Operator may create a new job later.

---

## 11. Lab stand-in (do not implement this)

The bench used a throwaway Python server and a public test broker. The lab board already speaks the production topics:

- Trigger: `tools/cam_lab_api/publish_trigger.py` → `test.mosquitto.org` topic `fleet/v1/{device_id}/camera/cmd`
- Ack: `fleet/v1/{device_id}/camera/ack`
- Upload: unauthenticated `POST /lab/camera/upload` behind an ngrok URL (not `/v1/camera/uploads`)

Production server uses **`fleet/v1/{device_id}/camera/...`** and `POST /v1/camera/uploads` with the device bearer. Backend should not depend on ngrok or on `test.mosquitto.org`.

---

## 12. Acceptance tests for backend

1. Create job for device A. MQTT test client subscribed only to A receives one message. A client subscribed to device B receives nothing.
2. Upload a real JPEG for that `job_id`. `GET .../image` returns the same bytes (SOI `FF D8`).
3. Second upload of the same `job_id` returns `200` and does not create a second object.
4. Upload with the wrong `device_id` returns `409` and stores nothing.
5. Upload larger than 2 MB or without JPEG SOI returns `400`.
6. Second `POST /jobs` for the same device while the first is `published` returns `409`.
7. No ack and no upload within 120 s → state `expired`.
8. Ack `fail` with no JPEG stored → state `failed`, `GET image` returns `409`. The same ack after the JPEG is stored leaves state `uploaded`.
9. Device token missing on upload → `401`.
10. MQTT publish failure → create API does not report success.

---

## 13. Open point for backend (does not block the API shape)

Device tokens: reuse the fleet’s existing device credential if one already exists for LTE calls. If none exists, add a per-`device_id` bearer and a provisioning step. The HTTP and MQTT contracts above stay the same either way.
