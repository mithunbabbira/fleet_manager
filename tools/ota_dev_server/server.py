#!/usr/bin/env python3
"""
Lab OTA manifest + .bin host for fleet-telematics-node.

Stdlib only (no pip). Serves:
  GET /health
  GET /firmware/manifest?device_id=...&channel=stable
  GET /firmware/files/<version>/<filename>

Place built images under:
  firmware/<version>/elm327_esp32c6.bin
  firmware/<version>/elm327_esp32c6.bin.sha256   (optional; auto-computed if missing)

Usage:
  cd tools/ota_dev_server
  python3 server.py --port 8080 --host 0.0.0.0
"""

from __future__ import annotations

import argparse
import hashlib
import json
import mimetypes
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent
FIRMWARE_DIR = ROOT / "firmware"
DEFAULT_CHANNEL = "stable"
BIN_NAME = "elm327_esp32c6.bin"

VERSION_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._-]{0,63}$")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def list_versions() -> list[str]:
    if not FIRMWARE_DIR.is_dir():
        return []
    versions = []
    for child in FIRMWARE_DIR.iterdir():
        if child.is_dir() and VERSION_RE.match(child.name) and (child / BIN_NAME).is_file():
            versions.append(child.name)
    # Semver-ish sort: split on non-digits, compare tuples when possible
    def key(v: str):
        parts = re.split(r"[^0-9]+", v)
        nums = tuple(int(p) for p in parts if p.isdigit())
        return (nums, v)

    return sorted(versions, key=key)


def latest_version(channel: str) -> str | None:
    # Channel can later map to a symlink or channel file; for lab, all channels → latest.
    _ = channel
    vers = list_versions()
    return vers[-1] if vers else None


def manifest_for(version: str, base_url: str) -> dict:
    bin_path = FIRMWARE_DIR / version / BIN_NAME
    if not bin_path.is_file():
        raise FileNotFoundError(version)

    sha_path = bin_path.with_suffix(bin_path.suffix + ".sha256")
    if sha_path.is_file():
        digest = sha_path.read_text(encoding="utf-8").strip().split()[0]
    else:
        digest = sha256_file(bin_path)

    size = bin_path.stat().st_size
    url = f"{base_url.rstrip('/')}/firmware/files/{version}/{BIN_NAME}"
    return {
        "version": version,
        "url": url,
        "sha256": digest,
        "size": size,
        "channel": DEFAULT_CHANNEL,
        "filename": BIN_NAME,
    }


class OtaHandler(BaseHTTPRequestHandler):
    server_version = "FleetOtaDevServer/1.0"

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, code: int, body: bytes, content_type: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code: int, obj: dict) -> None:
        raw = json.dumps(obj, indent=2).encode("utf-8") + b"\n"
        self._send(code, raw, "application/json; charset=utf-8")

    def _base_url(self) -> str:
        host = self.headers.get("Host") or f"localhost:{self.server.server_port}"
        # Prefer X-Forwarded if behind a tunnel later
        proto = self.headers.get("X-Forwarded-Proto", "http")
        return f"{proto}://{host}"

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"

        if path == "/health":
            self._json(200, {"ok": True, "versions": list_versions()})
            return

        if path == "/firmware/manifest":
            qs = parse_qs(parsed.query)
            channel = (qs.get("channel") or [DEFAULT_CHANNEL])[0]
            device_id = (qs.get("device_id") or [""])[0]
            version = (qs.get("version") or [None])[0]
            if not version:
                version = latest_version(channel)
            if not version:
                self._json(404, {"error": "no firmware published", "device_id": device_id})
                return
            try:
                m = manifest_for(version, self._base_url())
                m["device_id"] = device_id
                m["channel"] = channel
                self._json(200, m)
            except FileNotFoundError:
                self._json(404, {"error": "version not found", "version": version})
            return

        if path.startswith("/firmware/files/"):
            # /firmware/files/<version>/<filename>
            parts = path.split("/")
            # ['', 'firmware', 'files', version, filename]
            if len(parts) != 5:
                self._json(404, {"error": "bad file path"})
                return
            version, filename = parts[3], parts[4]
            if not VERSION_RE.match(version) or filename != BIN_NAME:
                self._json(404, {"error": "not found"})
                return
            file_path = FIRMWARE_DIR / version / filename
            if not file_path.is_file():
                self._json(404, {"error": "file missing"})
                return
            data = file_path.read_bytes()
            ctype = mimetypes.guess_type(filename)[0] or "application/octet-stream"
            self._send(200, data, ctype)
            return

        self._json(
            404,
            {
                "error": "not found",
                "hint": [
                    "GET /health",
                    "GET /firmware/manifest?device_id=fleet-demo-001&channel=stable",
                    f"GET /firmware/files/<version>/{BIN_NAME}",
                ],
            },
        )


def main() -> int:
    ap = argparse.ArgumentParser(description="Fleet lab OTA manifest + .bin host")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()

    FIRMWARE_DIR.mkdir(parents=True, exist_ok=True)
    httpd = ThreadingHTTPServer((args.host, args.port), OtaHandler)
    print(f"OTA lab server on http://{args.host}:{args.port}")
    print(f"Firmware root: {FIRMWARE_DIR}")
    print(f"Published versions: {list_versions() or '(none — copy a .bin under firmware/<ver>/)'}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
