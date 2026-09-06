#!/usr/bin/env python3
"""
Lab mock matching Trafyn fleet uplink (nc-fleet-device).

Mirrors production contract used by firmware_v2 master:

  POST /v1/sources/nc-fleet-device/messages
  Content-Type: application/json
  Body: bare JSON array of envelopes (NOT {"Vehicle":[...]}).

  Envelope:
    device_id, node_id, ts_ms, payload   — required
    device_type                          — required "obd"|"gps" for those kinds;
                                           omit for Zigbee/fuel hosts
    schemaId                             — optional (firmware still sends 1087/1088/1089)

  GET  /health
  GET  /           — live monitor
  GET  /events     — recent posts (?n=50&format=json)

Usage:
  python3 tools/telemetry_mock/server.py
  # Optional tunnel: ngrok http 8787
  # Lab only: point CONFIG_UPLINK_URL at the tunnel + /v1/sources/nc-fleet-device/messages
"""

from __future__ import annotations

import json
import sys
import threading
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

HOST = "0.0.0.0"
PORT = 8787
# Same path as production Trafyn uplink.
PATH = "/v1/sources/nc-fleet-device/messages"
LEGACY_PATH = "/nc-events-api/v2/messages"
LOG_PATH = Path(__file__).resolve().parent / "received.jsonl"
MAX_RECENT = 200

REQUIRED = ("device_id", "node_id", "ts_ms", "payload")

_lock = threading.Lock()
_recent: deque[dict[str, Any]] = deque(maxlen=MAX_RECENT)
_total_posts = 0
_total_events = 0


def _is_fuel_host(obj: dict[str, Any]) -> bool:
    payload = obj.get("payload")
    if isinstance(payload, dict) and isinstance(payload.get("host_type"), str):
        return True
    sid = obj.get("schemaId")
    return sid == "1088"


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
    if not isinstance(obj["ts_ms"], (int, float)):
        return f"events[{index}].ts_ms must be a number"
    if not isinstance(obj["payload"], dict):
        return f"events[{index}].payload must be an object"

    if "schemaId" in obj and (
        not isinstance(obj["schemaId"], str) or not obj["schemaId"]
    ):
        return f"events[{index}].schemaId must be a non-empty string when present"

    dtype = obj.get("device_type")
    sid = obj.get("schemaId")
    fuel = _is_fuel_host(obj)

    if fuel:
        # Zigbee/fuel: device_type must be omitted (backend keys by device_id).
        if dtype is not None and dtype != "":
            return (
                f"events[{index}] fuel/host envelope must omit device_type "
                f"(got {dtype!r})"
            )
        return None

    if dtype is not None and (not isinstance(dtype, str) or not dtype):
        return f"events[{index}].device_type must be a non-empty string when present"

    # OBD / GPS require device_type (config-backed in firmware).
    expect: str | None = None
    if sid == "1087" or dtype == "obd":
        expect = "obd"
    elif sid == "1089" or dtype == "gps":
        expect = "gps"
    elif isinstance(obj.get("payload"), dict) and "gps_ok" in obj["payload"]:
        expect = "gps"
    elif isinstance(obj.get("payload"), dict) and (
        "rpm" in obj["payload"] or "obd_profile" in obj["payload"]
    ):
        expect = "obd"

    if expect is not None:
        if dtype != expect:
            return (
                f"events[{index}] requires device_type={expect!r} "
                f"(got {dtype!r})"
            )

    return None


def extract_events(body: Any) -> tuple[list[Any] | None, str | None]:
    """Production shape: bare JSON array only."""
    if isinstance(body, list):
        return body, None
    if isinstance(body, dict) and "Vehicle" in body:
        return None, (
            'body must be a bare JSON array [...]; '
            '{"Vehicle":[...]} wrap is not accepted on nc-fleet-device'
        )
    return None, "body must be a non-empty JSON array of envelopes"


def remember(record: dict[str, Any]) -> None:
    global _total_posts, _total_events
    with _lock:
        _recent.appendleft(record)
        _total_posts += 1
        _total_events += int(record.get("count") or 0)


def snapshot(n: int = 50) -> dict[str, Any]:
    with _lock:
        items = list(_recent)[: max(1, min(n, MAX_RECENT))]
        return {
            "ok": True,
            "total_posts": _total_posts,
            "total_events": _total_events,
            "showing": len(items),
            "posts": items,
            "api": PATH,
        }


MONITOR_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>Fleet telemetry mock (nc-fleet-device)</title>
  <style>
    :root { color-scheme: light dark; font-family: ui-sans-serif, system-ui, sans-serif; }
    body { margin: 1.25rem; line-height: 1.4; color: #111; background: #fafafa; }
    @media (prefers-color-scheme: dark) {
      body { color: #eee; background: #121212; }
    }
    h1 { font-size: 1.25rem; margin: 0 0 0.5rem; }
    .meta { opacity: 0.75; font-size: 0.9rem; margin-bottom: 1rem; }
    .stats { display: flex; gap: 1rem; flex-wrap: wrap; margin-bottom: 1rem; }
    .stat { border: 1px solid #8884; border-radius: 8px; padding: 0.6rem 0.9rem; min-width: 7rem; }
    .stat b { display: block; font-size: 1.2rem; }
    article { border: 1px solid #8884; border-radius: 10px; padding: 0.75rem 1rem; margin: 0.75rem 0;
              background: #fff; }
    @media (prefers-color-scheme: dark) {
      article { background: #1c1c1c; }
    }
    .hdr { display: flex; flex-wrap: wrap; gap: 0.75rem; font-size: 0.9rem; margin-bottom: 0.5rem; }
    .chip { background: #8882; padding: 0.15rem 0.5rem; border-radius: 999px; }
    .ev { margin: 0.65rem 0; padding-top: 0.35rem; border-top: 1px solid #8883; }
    pre { margin: 0.35rem 0 0; padding: 0.6rem; overflow: auto; font-size: 0.8rem;
          background: #8881; border-radius: 6px; white-space: pre-wrap; word-break: break-word; }
    .empty { opacity: 0.6; padding: 2rem 0; }
    .err { color: #c62828; }
  </style>
</head>
<body>
  <h1>Fleet telemetry mock</h1>
  <div class="meta">Mirrors Trafyn <code>POST /v1/sources/nc-fleet-device/messages</code>
    (bare array · device_type obd/gps · fuel omits type) · auto-refresh 2s ·
    JSON: <a href="/events?n=20&amp;format=json">/events?format=json</a> ·
    <a href="/health">/health</a></div>
  <div class="stats">
    <div class="stat"><span>Posts</span><b id="stat-posts">0</b></div>
    <div class="stat"><span>Events</span><b id="stat-events">0</b></div>
    <div class="stat"><span>Updated</span><b id="stat-updated" style="font-size:0.95rem">—</b></div>
  </div>
  <div id="list" class="empty">Waiting for device POSTs…</div>
  <script>
    function esc(s) {
      return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
    }
    async function refresh() {
      const updated = document.getElementById('stat-updated');
      try {
        const r = await fetch('/events?n=40&format=json');
        if (!r.ok) throw new Error('HTTP ' + r.status);
        const d = await r.json();
        document.getElementById('stat-posts').textContent = d.total_posts;
        document.getElementById('stat-events').textContent = d.total_events;
        updated.textContent = new Date().toLocaleTimeString();
        updated.className = '';
        const list = document.getElementById('list');
        if (!d.posts || !d.posts.length) {
          list.className = 'empty';
          list.textContent = 'Waiting for device POSTs…';
          return;
        }
        list.className = '';
        list.innerHTML = d.posts.map(p => {
          const evs = (p.events || []).map(ev => {
            const pay = esc(JSON.stringify(ev.payload || {}, null, 2));
            const dtype = ev.device_type ? esc(ev.device_type) : '—';
            const sid = ev.schemaId ? esc(ev.schemaId) : '—';
            return `<div class="ev">
              <span class="chip">type ${dtype}</span>
              <span class="chip">schema ${sid}</span>
              <div><code>${esc(ev.device_id)}</code> / <code>${esc(ev.node_id)}</code> · ts=${esc(ev.ts_ms)}</div>
              <pre>${pay}</pre>
            </div>`;
          }).join('');
          return `<article>
            <div class="hdr">
              <span class="chip">${esc(p.received_at || '')}</span>
              <span class="chip">count=${esc(p.count)}</span>
              <span class="chip">${esc(p.remote || '')}</span>
            </div>${evs}
          </article>`;
        }).join('');
      } catch (e) {
        updated.textContent = 'error: ' + e;
        updated.className = 'err';
      }
    }
    refresh();
    setInterval(refresh, 2000);
  </script>
</body>
</html>
"""


def _wants_html(handler: BaseHTTPRequestHandler) -> bool:
    qs = parse_qs(urlparse(handler.path).query)
    fmt = (qs.get("format") or [""])[0].lower()
    if fmt in ("json", "raw"):
        return False
    if fmt in ("html", "ui"):
        return True
    accept = (handler.headers.get("Accept") or "").lower()
    if "application/json" in accept and "text/html" not in accept:
        return False
    if "text/html" in accept:
        return True
    return "Mozilla" in (handler.headers.get("User-Agent") or "")


def events_html(data: dict[str, Any]) -> str:
    parts = [
        "<!DOCTYPE html><html><head><meta charset='utf-8'/>",
        "<title>events</title>",
        "<style>body{font-family:ui-sans-serif,system-ui,sans-serif;margin:1.25rem;}",
        "pre{white-space:pre-wrap;word-break:break-word;background:#f4f4f4;padding:1rem;",
        "border-radius:8px;} a{margin-right:1rem}</style></head><body>",
        "<p><a href='/'>Live monitor UI</a>",
        "<a href='/events?n=20&amp;format=json'>JSON</a></p>",
        f"<p>api={data.get('api')} total_posts={data.get('total_posts')} "
        f"total_events={data.get('total_events')} showing={data.get('showing')}</p>",
        "<pre>",
        json.dumps(data, indent=2).replace("&", "&amp;").replace("<", "&lt;"),
        "</pre></body></html>",
    ]
    return "".join(parts)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, code: int, obj: dict) -> None:
        raw = json.dumps(obj, indent=2).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _send_html(self, code: int, html: str) -> None:
        raw = html.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"
        if path == "/health":
            self._send(200, {"ok": True, **snapshot(5)})
            return
        if path == "/events":
            qs = parse_qs(parsed.query)
            n = 50
            if "n" in qs:
                try:
                    n = int(qs["n"][0])
                except ValueError:
                    n = 50
            data = snapshot(n)
            if _wants_html(self):
                self._send_html(200, events_html(data))
            else:
                self._send(200, data)
            return
        if path == "/":
            self._send_html(200, MONITOR_HTML)
            return
        self._send(404, {"ok": False, "error": "not found"})

    def do_POST(self) -> None:
        path = urlparse(self.path).path.rstrip("/") or "/"
        if path == LEGACY_PATH.rstrip("/"):
            self._send(
                404,
                {
                    "ok": False,
                    "error": f"legacy path retired; use POST {PATH}",
                },
            )
            return
        if path != PATH.rstrip("/"):
            self._send(404, {"ok": False, "error": f"use POST {PATH}"})
            return

        length = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(length) if length > 0 else b""
        try:
            body = json.loads(raw.decode("utf-8") if raw else "null")
        except json.JSONDecodeError as e:
            self._send(400, {"ok": False, "error": f"invalid json: {e}"})
            return

        events, err = extract_events(body)
        if err:
            self._send(400, {"ok": False, "error": err})
            return
        assert events is not None
        if len(events) == 0:
            self._send(400, {"ok": False, "error": "array must not be empty"})
            return

        for i, ev in enumerate(events):
            verr = validate_envelope(ev, i)
            if verr:
                self._send(400, {"ok": False, "error": verr})
                return

        record = {
            "received_at": datetime.now(timezone.utc).isoformat(),
            "remote": self.client_address[0],
            "count": len(events),
            "events": events,
        }
        with LOG_PATH.open("a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")
        remember(record)

        print(f"accepted {len(events)} event(s):", flush=True)
        for ev in events:
            print(
                f"  type={ev.get('device_type') or '-'} schemaId={ev.get('schemaId') or '-'} "
                f"device_id={ev['device_id']} node_id={ev['node_id']} ts_ms={ev['ts_ms']}",
                flush=True,
            )

        self._send(200, {"ok": True, "accepted": len(events)})


def main() -> None:
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"telemetry mock on http://127.0.0.1:{PORT}/  (monitor UI)", flush=True)
    print(f"POST {PATH}  (production-shaped)", flush=True)
    print(f"logging to {LOG_PATH}", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye", flush=True)


if __name__ == "__main__":
    main()
