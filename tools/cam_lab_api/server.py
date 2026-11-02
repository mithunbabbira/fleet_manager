#!/usr/bin/env python3
"""
DELETE ME — throwaway lab API for cabin-camera JPEG uploads.

  POST /lab/camera/upload   multipart: file (JPEG) + job_id (+ optional deg, device_id)
  GET  /lab/camera/<job_id>  return saved JPEG (latest for that job_id)
  GET  /health
  GET  /                     short help

Saves under uploads/cam_lab/ (gitignored). Not Trafyn. Not production.

Usage:
  cd tools/cam_lab_api && python3 server.py
  # Mac must be on Fleet-C6 SoftAP (or reachable from C6 STA)
"""

from __future__ import annotations

import json
import re
import threading
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

HOST = "0.0.0.0"
PORT = 8790
ROOT = Path(__file__).resolve().parent
UPLOAD_DIR = ROOT / "uploads" / "cam_lab"
INDEX_PATH = UPLOAD_DIR / "_index.json"

_lock = threading.Lock()
_job_files: dict[str, str] = {}


def _load_index() -> None:
    global _job_files
    if not INDEX_PATH.is_file():
        _job_files = {}
        return
    try:
        _job_files = json.loads(INDEX_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        _job_files = {}


def _save_index() -> None:
    UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
    INDEX_PATH.write_text(json.dumps(_job_files, indent=2), encoding="utf-8")


def _safe_job_id(raw: str) -> str | None:
    jid = (raw or "").strip()
    if not jid or len(jid) > 64:
        return None
    if not re.fullmatch(r"[A-Za-z0-9._-]+", jid):
        return None
    return jid


def _parse_multipart(body: bytes, content_type: str) -> dict[str, bytes | str]:
    """Minimal multipart/form-data parser (file + text fields)."""
    m = re.search(r"boundary=([^;]+)", content_type, re.I)
    if not m:
        raise ValueError("missing multipart boundary")
    boundary = m.group(1).strip().strip('"').encode("ascii", "ignore")
    if not boundary:
        raise ValueError("empty boundary")
    parts = body.split(b"--" + boundary)
    out: dict[str, bytes | str] = {}
    for part in parts:
        if not part or part in (b"--\r\n", b"--", b"\r\n"):
            continue
        if part.startswith(b"--"):
            continue
        if part.startswith(b"\r\n"):
            part = part[2:]
        if part.endswith(b"\r\n"):
            part = part[:-2]
        header_blob, _, data = part.partition(b"\r\n\r\n")
        if not _:
            continue
        if data.endswith(b"\r\n"):
            data = data[:-2]
        headers = header_blob.decode("utf-8", "replace")
        name_m = re.search(r'name="([^"]+)"', headers, re.I)
        if not name_m:
            continue
        name = name_m.group(1)
        if "filename=" in headers.lower():
            out[name] = data
        else:
            out[name] = data.decode("utf-8", "replace")
    return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args) -> None:
        print(f"[cam_lab_api] {self.address_string()} {fmt % args}")

    def _send(self, code: int, body: bytes, content_type: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, code: int, obj: dict) -> None:
        raw = (json.dumps(obj) + "\n").encode("utf-8")
        self._send(code, raw, "application/json")

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path == "/health":
            self._send_json(200, {"ok": True, "service": "cam_lab_api", "delete_me": True})
            return
        if path in ("/", "/help"):
            text = (
                "DELETE ME — cam_lab_api\n"
                "POST /lab/camera/upload  multipart file + job_id\n"
                "GET  /lab/camera/<job_id>\n"
                "GET  /health\n"
            ).encode("utf-8")
            self._send(200, text, "text/plain; charset=utf-8")
            return
        if path.startswith("/lab/camera/"):
            jid = _safe_job_id(unquote(path[len("/lab/camera/") :]))
            if not jid:
                self._send_json(400, {"ok": False, "error": "bad job_id"})
                return
            with _lock:
                rel = _job_files.get(jid)
            if not rel:
                self._send_json(404, {"ok": False, "error": "unknown job_id"})
                return
            fpath = UPLOAD_DIR / Path(rel).name
            if not fpath.is_file():
                self._send_json(404, {"ok": False, "error": "file missing"})
                return
            data = fpath.read_bytes()
            self._send(200, data, "image/jpeg")
            return
        self._send_json(404, {"ok": False, "error": "not found"})

    def do_POST(self) -> None:
        path = urlparse(self.path).path
        if path != "/lab/camera/upload":
            self._send_json(404, {"ok": False, "error": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length <= 0 or length > 2_000_000:
            self._send_json(400, {"ok": False, "error": "bad Content-Length"})
            return
        ctype = self.headers.get("Content-Type", "")
        body = self.rfile.read(length)
        try:
            fields = _parse_multipart(body, ctype)
        except ValueError as e:
            self._send_json(400, {"ok": False, "error": str(e)})
            return
        jid = _safe_job_id(str(fields.get("job_id", "")))
        if not jid:
            self._send_json(400, {"ok": False, "error": "job_id required"})
            return
        raw = fields.get("file")
        if not isinstance(raw, (bytes, bytearray)) or len(raw) < 100:
            self._send_json(400, {"ok": False, "error": "file (JPEG) required"})
            return
        data = bytes(raw)
        if data[:2] != b"\xff\xd8":
            self._send_json(400, {"ok": False, "error": "not a JPEG (missing SOI)"})
            return
        deg = str(fields.get("deg", "")).strip()
        device_id = str(fields.get("device_id", "lab")).strip() or "lab"
        ts = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        fname = f"{jid}_{ts}.jpg"
        UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
        fpath = UPLOAD_DIR / fname
        fpath.write_bytes(data)
        with _lock:
            _job_files[jid] = fname
            _save_index()
        print(
            f"[cam_lab_api] saved job_id={jid} device={device_id} deg={deg} "
            f"bytes={len(data)} -> {fpath}"
        )
        self._send_json(
            200,
            {
                "ok": True,
                "job_id": jid,
                "bytes": len(data),
                "path": f"uploads/cam_lab/{fname}",
                "url": f"/lab/camera/{jid}",
                "delete_me": True,
            },
        )


def main() -> None:
    UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
    _load_index()
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"cam_lab_api DELETE-ME listening http://{HOST}:{PORT}")
    print(f"uploads -> {UPLOAD_DIR}")
    print("POST /lab/camera/upload  GET /lab/camera/<job_id>")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
