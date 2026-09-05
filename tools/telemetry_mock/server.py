#!/usr/bin/env python3
"""
Lab mock for fleet telemetry POST (array-only bare envelopes).

  POST /nc-events-api/v2/messages
  GET  /health

Usage:
  python3 tools/telemetry_mock/server.py
  ngrok http 8787
  # Point CONFIG_UPLINK_URL at https://<ngrok>/nc-events-api/v2/messages
"""

from __future__ import annotations

import json
import sys
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

HOST = "0.0.0.0"
PORT = 8787
PATH = "/nc-events-api/v2/messages"
LOG_PATH = Path(__file__).resolve().parent / "received.jsonl"

REQUIRED = ("device_id", "node_id", "schemaId", "ts_ms", "payload")


def validate_envelope(obj: Any, index: int) -> str | None:
    if not isinstance(obj, dict):
        return f"events[{index}] must be an object"
    for key in REQUIRED:
        if key not in obj:
            return f"events[{index}] missing '{key}'"
    if not isinstance(obj["device_id"], str) or not obj["device_id"]:
        return f"events[{index}].device_id must be a non-empty string"
    if not isinstance(obj["node_id"], str) or not obj["node_id"]:
        return f"events[{index}].node_id must be a non-empty string"
    if not isinstance(obj["schemaId"], str) or not obj["schemaId"]:
        return f"events[{index}].schemaId must be a non-empty string"
    if not isinstance(obj["ts_ms"], (int, float)):
        return f"events[{index}].ts_ms must be a number"
    if not isinstance(obj["payload"], dict):
        return f"events[{index}].payload must be an object"
    if "Vehicle" in obj:
        return f"events[{index}] must be a bare envelope (no 'Vehicle' key)"
    return None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, code: int, obj: dict) -> None:
        raw = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self) -> None:
        path = urlparse(self.path).path.rstrip("/") or "/"
        if path == "/health":
            self._send(200, {"ok": True})
            return
        self._send(404, {"ok": False, "error": "not found"})

    def do_POST(self) -> None:
        path = urlparse(self.path).path
        if path.rstrip("/") != PATH.rstrip("/"):
            self._send(404, {"ok": False, "error": f"use POST {PATH}"})
            return

        length = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(length) if length > 0 else b""
        try:
            body = json.loads(raw.decode("utf-8") if raw else "null")
        except json.JSONDecodeError as e:
            self._send(400, {"ok": False, "error": f"invalid json: {e}"})
            return

        if not isinstance(body, list):
            self._send(
                400,
                {
                    "ok": False,
                    "error": "body must be a JSON array of envelopes (even for one event)",
                },
            )
            return
        if len(body) == 0:
            self._send(400, {"ok": False, "error": "array must not be empty"})
            return

        for i, ev in enumerate(body):
            err = validate_envelope(ev, i)
            if err:
                self._send(400, {"ok": False, "error": err})
                return

        record = {
            "received_at": datetime.now(timezone.utc).isoformat(),
            "remote": self.client_address[0],
            "count": len(body),
            "events": body,
        }
        with LOG_PATH.open("a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")

        print(f"accepted {len(body)} event(s):", flush=True)
        for ev in body:
            print(
                f"  schemaId={ev['schemaId']} device_id={ev['device_id']} "
                f"node_id={ev['node_id']} ts_ms={ev['ts_ms']}",
                flush=True,
            )

        self._send(200, {"ok": True, "accepted": len(body)})


def main() -> None:
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"telemetry mock on http://{HOST}:{PORT}{PATH}", flush=True)
    print(f"logging to {LOG_PATH}", flush=True)
    print("ngrok: ngrok http 8787", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye", flush=True)


if __name__ == "__main__":
    main()
