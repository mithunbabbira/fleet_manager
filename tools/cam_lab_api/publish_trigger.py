#!/usr/bin/env python3
"""
DELETE ME — publish a desk MQTT capture trigger for master_lab.

Requires a broker (local Mosquitto recommended):
  brew install mosquitto && brew services start mosquitto

Usage (Mac on Fleet-C6 SoftAP, broker on this Mac):
  python3 publish_trigger.py --host 192.168.4.2 --deg 90
  python3 publish_trigger.py --host 127.0.0.1 --deg 45 --job-id desk1
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import uuid

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("pip install -r requirements.txt  # needs paho-mqtt", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    ap = argparse.ArgumentParser(description="Lab MQTT cam trigger (DELETE ME)")
    ap.add_argument("--host", default="127.0.0.1", help="MQTT broker host")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--device-id", required=True, help="provisioned device_id (NVS elm/uplink_did)")
    ap.add_argument("--deg", type=int, default=90)
    ap.add_argument("--job-id", default="")
    args = ap.parse_args()

    deg = max(0, min(180, args.deg))
    job_id = args.job_id.strip() or f"job-{uuid.uuid4().hex[:10]}"
    topic = f"fleet/v1/{args.device_id}/camera/cmd"
    payload = json.dumps({"job_id": job_id, "deg": deg})

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"cam-lab-pub-{job_id}")
    client.connect(args.host, args.port, keepalive=30)
    client.loop_start()
    info = client.publish(topic, payload, qos=1)
    info.wait_for_publish(timeout=5)
    client.loop_stop()
    client.disconnect()
    print(f"published {topic} -> {payload}")
    print(f"expect upload then: curl -o shot.jpg http://127.0.0.1:8790/lab/camera/{job_id}")
    time.sleep(0.1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
