# DELETE ME — Phase 2: EC200U MQTT trigger (field-like)

Desk path uses **Wi‑Fi MQTT** from SoftAP stations (Mac Mosquitto → C6 `PubSubClient`).

Field-like path uses the **Quectel EC200U** MQTT AT set on the production UART
(GPIO16 TX / GPIO17 RX). Do **not** wire this into `fleet_v2_master` until the
desk loop is proven. Same job semantics: MQTT cmd → capture → **HTTP** upload
(JPEG stays on QHTTP / `cam_lab_api`, not MQTT).

## Prerequisites

1. PDP up (`AT+QIACT=1`) — same as existing QHTTP uplink.
2. Broker reachable over LTE (public Mosquitto, HiveMQ Cloud, or tunnel).
3. Expose `cam_lab_api` via tunnel for upload, e.g. `cloudflared` / `ngrok http 8790`.

## AT sketch (EC200U)

Adjust client-idx / SSL options per your modem FW (Quectel EC200U MQTT AT manual).

```text
AT+QMTCFG="recv/mode",0,0,1
AT+QMTOPEN=0,"broker.example.com",1883
AT+QMTCONN=0,"lab-c6","user","pass"
AT+QMTSUB=0,1,"fleet/lab/lab-c6/cam/cmd",1
```

On `+QMTRECV` with payload `{"job_id":"...","deg":90}`:

1. Run the same capture job as USB/`PubSubClient` (SoftAP → CAM).
2. Upload JPEG with existing QHTTP multipart POST to the tunnel URL
   (mirror `uploadJpeg()` in master_lab, but through `lte` / QHTTP).
3. Optional: `AT+QMTPUBEX` ACK on `fleet/lab/lab-c6/cam/ack`.

## Why not MQTT for the image?

~30–40 KiB JPEG on LTE MQTT is fragile (payload limits, buffering, cost).  
Trigger = MQTT; bytes = HTTP.

## Lab probe (optional)

With EC200U USB AT port attached to a PC (not the SoftAP lab C6 sketch):

```bash
# interactive AT from repo tools
python3 tools/ec200u_at_probe.py   # if present; else use serial monitor
```

Paste the `QMTOPEN` / `QMTCONN` / `QMTSUB` sequence above once PDP is active.

## Integration later

When ready: add a `LAB_EC200U_MQTT` path next to desk Wi‑Fi MQTT in a **spare**
sketch, or gate behind Kconfig in production — only after desk
`job` + `publish_trigger.py` + `uploads/cam_lab/` are green.
