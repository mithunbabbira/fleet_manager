# DELETE ME — cabin camera lab API + MQTT trigger

**Internet on C6 = EC200U LTE** (not home Wi‑Fi). SoftAP is only for the CAM.

```
Mac: cam_lab_api :8790 + ngrok http 8790
         │ HTTPS upload URL
         ▼
EC200U LTE ◄── UART16/17 ──► C6 master_lab SoftAP ──► ESP32-CAM
         ▲
         └── MQTT test.mosquitto.org (QMT*) trigger
C6 USB: lte | ltemqtt | uploadurl | job
```

Not Trafyn. Not `fleet_v2_master`. Delete this folder when real jobs land.

## 1) API + ngrok (public URL required for LTE)

```bash
cd tools/cam_lab_api
python3 server.py
ngrok http 8790
# copy https://….ngrok-free.app
```

## 2) Flash spare C6 + set upload URL

```bash
cd firmware_v2/camera/master_lab
pio run -e master_lab_c6 -t upload --upload-port /dev/cu.usbmodem*
```

USB @ 115200:

```text
lte
ltemqtt
uploadurl https://YOUR.ngrok-free.app/lab/camera/upload
ping
job 90 lte1
```

Expect `lte_ready=1`, `CGATT: 1`, `+QIACT: …`, then `LTE upload OK`.

## 3) MQTT trigger over LTE

```bash
pip3 install -r requirements.txt
python3 publish_trigger.py --host test.mosquitto.org --deg 90 --job-id mqtt1
curl -o shot.jpg -H "ngrok-skip-browser-warning: 1" https://YOUR.ngrok-free.app/lab/camera/mqtt1
```

Topic: `fleet/lab/lab-c6/cam/cmd`

## API

- `POST /lab/camera/upload` — multipart `file` + `job_id`
- `GET /lab/camera/<job_id>`
- `GET /health`

## Phase 2 (LTE)

See [ec200u_mqtt_phase2.md](ec200u_mqtt_phase2.md) — EC200U `QMT*` + QHTTP upload via tunnel.
