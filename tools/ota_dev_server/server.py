#!/usr/bin/env python3
"""
Lab OTA manifest + .bin host for fleet-telematics-node.

Stdlib only (no pip). Serves:
  GET /health
  GET /firmware/manifest?device_id=...&channel=stable
  GET /firmware/active/elm327_esp32c6.bin
  GET /firmware/files/<version>/<filename>

Preferred config (edit anytime; re-read each request):
  tools/ota_dev_server/release.json
  {
    "version": "1.0.5-lab",
    "bin": "../../build/elm327_esp32c6.bin",
    "channel": "stable"
  }

`version` is returned as-is in GET /firmware/manifest.

Fallback if release.json missing: firmware/<version>/elm327_esp32c6.bin (latest).

Usage:
  cd tools/ota_dev_server
  python3 server.py --port 8080 --host 0.0.0.0
"""

from __future__ import annotations

import argparse
import hashlib
import json
import mimetypes
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent
FIRMWARE_DIR = ROOT / "firmware"
RELEASE_JSON = ROOT / "release.json"
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

    def key(v: str):
        parts = re.split(r"[^0-9]+", v)
        nums = tuple(int(p) for p in parts if p.isdigit())
        return (nums, v)

    return sorted(versions, key=key)


def latest_version(channel: str) -> str | None:
    _ = channel
    vers = list_versions()
    return vers[-1] if vers else None


def load_release_config() -> dict | None:
    """Return {version, bin_path: Path, channel} — version comes from release.json as-is."""
    if not RELEASE_JSON.is_file():
        return None
    try:
        raw = json.loads(RELEASE_JSON.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        sys.stderr.write(f"release.json read error: {e}\n")
        return None

    version = str(raw.get("version") or "").strip()
    bin_field = str(raw.get("bin") or "").strip()
    channel = str(raw.get("channel") or DEFAULT_CHANNEL).strip() or DEFAULT_CHANNEL
    if not version or not VERSION_RE.match(version) or not bin_field:
        sys.stderr.write("release.json: need version + bin\n")
        return None

    bin_path = Path(bin_field)
    if not bin_path.is_absolute():
        bin_path = (ROOT / bin_path).resolve()
    if not bin_path.is_file():
        sys.stderr.write(f"release.json: bin not found: {bin_path}\n")
        return None

    return {"version": version, "bin_path": bin_path, "channel": channel}


def resolve_bin(version: str | None, channel: str) -> tuple[str, Path] | None:
    """Pick (version, bin_path). Prefer release.json unless ?version= overrides to folder."""
    cfg = load_release_config()

    if version:
        folder = FIRMWARE_DIR / version / BIN_NAME
        if folder.is_file():
            return version, folder
        if cfg and cfg["version"] == version:
            return cfg["version"], cfg["bin_path"]
        return None

    if cfg:
        return cfg["version"], cfg["bin_path"]

    ver = latest_version(channel)
    if not ver:
        return None
    return ver, FIRMWARE_DIR / ver / BIN_NAME


def active_bin_url(base_url: str) -> str:
    return f"{base_url.rstrip('/')}/firmware/active/{BIN_NAME}"


def manifest_for(
    version: str,
    bin_path: Path,
    base_url: str,
    channel: str,
    scenario: str = "",
) -> dict:
    digest = sha256_file(bin_path)
    size = bin_path.stat().st_size
    url = active_bin_url(base_url)

    if scenario == "bad_sha":
        last = digest[-1]
        digest = digest[:-1] + ("0" if last != "0" else "1")
    elif scenario == "truncate":
        url = f"{url}?scenario=truncate"
    elif scenario == "gone":
        raise FileNotFoundError("gone")

    return {
        "version": version,
        "url": url,
        "sha256": digest,
        "size": size,
        "channel": channel,
        "filename": BIN_NAME,
    }


class OtaHandler(BaseHTTPRequestHandler):
    server_version = "FleetOtaDevServer/1.1"

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
        proto = self.headers.get("X-Forwarded-Proto", "http")
        return f"{proto}://{host}"

    def _serve_bin(self, file_path: Path, scenario: str = "") -> None:
        if not file_path.is_file():
            self._json(404, {"error": "file missing"})
            return
        data = file_path.read_bytes()
        if scenario == "truncate":
            data = data[: max(1, len(data) // 2)]
        ctype = mimetypes.guess_type(file_path.name)[0] or "application/octet-stream"
        self._send(200, data, ctype)

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"

        if path == "/health":
            cfg = load_release_config()
            self._json(
                200,
                {
                    "ok": True,
                    "release_json": (
                        {
                            "version": cfg["version"],
                            "bin": str(cfg["bin_path"]),
                            "channel": cfg["channel"],
                        }
                        if cfg
                        else None
                    ),
                    "versions": list_versions(),
                },
            )
            return

        if path == "/firmware/manifest":
            qs = parse_qs(parsed.query)
            channel = (qs.get("channel") or [DEFAULT_CHANNEL])[0]
            device_id = (qs.get("device_id") or [""])[0]
            scenario = (qs.get("scenario") or [""])[0]
            version_q = (qs.get("version") or [None])[0]
            if scenario == "gone":
                self._json(404, {"error": "no_firmware", "device_id": device_id, "scenario": scenario})
                return
            resolved = resolve_bin(version_q, channel)
            if not resolved:
                self._json(404, {"error": "no firmware published", "device_id": device_id})
                return
            version, bin_path = resolved
            cfg = load_release_config()
            if cfg and version == cfg["version"]:
                channel = (qs.get("channel") or [cfg["channel"]])[0]
            try:
                m = manifest_for(version, bin_path, self._base_url(), channel, scenario=scenario)
                m["device_id"] = device_id
                if scenario:
                    m["scenario"] = scenario
                self._json(200, m)
            except FileNotFoundError:
                self._json(404, {"error": "version not found", "version": version})
            return

        if path == f"/firmware/active/{BIN_NAME}":
            qs = parse_qs(parsed.query)
            scenario = (qs.get("scenario") or [""])[0]
            resolved = resolve_bin(None, DEFAULT_CHANNEL)
            if not resolved:
                self._json(404, {"error": "no active firmware (set release.json or publish a version)"})
                return
            self._serve_bin(resolved[1], scenario=scenario)
            return

        if path.startswith("/firmware/files/"):
            parts = path.split("/")
            if len(parts) != 5:
                self._json(404, {"error": "bad file path"})
                return
            version, filename = parts[3], parts[4]
            if not VERSION_RE.match(version) or filename != BIN_NAME:
                self._json(404, {"error": "not found"})
                return
            file_path = FIRMWARE_DIR / version / filename
            qs = parse_qs(parsed.query)
            scenario = (qs.get("scenario") or [""])[0]
            self._serve_bin(file_path, scenario=scenario)
            return

        self._json(
            404,
            {
                "error": "not found",
                "hint": [
                    "GET /health",
                    "GET /firmware/manifest?device_id=fleet-demo-001&channel=stable",
                    "GET /firmware/manifest?scenario=bad_sha|truncate|gone",
                    f"GET /firmware/active/{BIN_NAME}",
                    f"GET /firmware/files/<version>/{BIN_NAME}",
                    "Edit tools/ota_dev_server/release.json (version + bin path)",
                ],
            },
        )


def main() -> int:
    ap = argparse.ArgumentParser(description="Fleet lab OTA manifest + .bin host")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()

    FIRMWARE_DIR.mkdir(parents=True, exist_ok=True)
    cfg = load_release_config()
    httpd = ThreadingHTTPServer((args.host, args.port), OtaHandler)
    print(f"OTA lab server on http://{args.host}:{args.port}")
    print(f"release.json: {RELEASE_JSON}")
    if cfg:
        print(f"  active version={cfg['version']} bin={cfg['bin_path']}")
    else:
        print(f"  (no valid release.json — using firmware/ folders)")
        print(f"  Published versions: {list_versions() or '(none)'}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
