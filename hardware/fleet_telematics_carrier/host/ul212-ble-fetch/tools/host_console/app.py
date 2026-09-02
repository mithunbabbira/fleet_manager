#!/usr/bin/env python3
"""Local web console for UL212 host ESP32 — serial bridge only.

    pip install -r tools/host_console/requirements.txt
    python3 tools/host_console/app.py
    python3 tools/host_console/app.py --port /dev/cu.usbmodem1201

Opens http://127.0.0.1:8765 — ESP32 stays serial-only; all UI runs on the PC.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import signal
import threading
import time
import webbrowser
from pathlib import Path
from typing import Any

import serial
import serial.tools.list_ports
from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
import uvicorn

BAUD = 115200
STATIC = Path(__file__).resolve().parent / "static"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_HTTP = 8765


class SerialBridge:
    """One open serial port; fan-out lines to WebSocket clients."""

    def __init__(self) -> None:
        self._ser: serial.Serial | None = None
        self._port: str | None = None
        self._clients: set[WebSocket] = set()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._reader: threading.Thread | None = None
        self._stop = threading.Event()
        self._lock = threading.Lock()

    @property
    def connected(self) -> bool:
        return self._ser is not None and self._ser.is_open

    @property
    def port(self) -> str | None:
        return self._port

    def set_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop

    def connect(self, port: str) -> None:
        self.disconnect()
        with self._lock:
            self._ser = serial.Serial(port, BAUD, timeout=0.15)
            self._port = port
            self._stop.clear()
            self._reader = threading.Thread(target=self._read_loop, daemon=True)
            self._reader.start()
        self._schedule_broadcast({"type": "system", "line": f"Connected to {port} @ {BAUD}"})

    def disconnect(self) -> None:
        self._stop.set()
        with self._lock:
            if self._ser and self._ser.is_open:
                try:
                    self._ser.close()
                except Exception:
                    pass
            self._ser = None
            old = self._port
            self._port = None
        if old:
            self._schedule_broadcast({"type": "system", "line": f"Disconnected from {old}"})

    def send_line(self, line: str) -> None:
        with self._lock:
            if not self._ser or not self._ser.is_open:
                raise RuntimeError("not connected")
            self._ser.write((line.strip() + "\n").encode())
            self._ser.flush()
        self._schedule_broadcast({"type": "tx", "line": line.strip()})

    def _read_loop(self) -> None:
        buf = ""
        while not self._stop.is_set():
            with self._lock:
                ser = self._ser
            if not ser or not ser.is_open:
                break
            try:
                chunk = ser.read(4096)
            except Exception as exc:
                self._schedule_broadcast({"type": "system", "line": f"Serial error: {exc}"})
                break
            if not chunk:
                continue
            buf += chunk.decode("utf-8", errors="replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.rstrip("\r")
                if line:
                    self._schedule_broadcast({"type": "rx", "line": line})

    def _schedule_broadcast(self, msg: dict[str, Any]) -> None:
        if not self._loop:
            return
        asyncio.run_coroutine_threadsafe(self.broadcast(msg), self._loop)

    async def add_client(self, ws: WebSocket) -> None:
        self._clients.add(ws)

    async def remove_client(self, ws: WebSocket) -> None:
        self._clients.discard(ws)

    async def broadcast(self, msg: dict[str, Any]) -> None:
        dead: list[WebSocket] = []
        payload = json.dumps(msg)
        for ws in list(self._clients):
            try:
                await ws.send_text(payload)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self._clients.discard(ws)


bridge = SerialBridge()
app = FastAPI(title="UL212 Host Console", docs_url=None, redoc_url=None)


@app.on_event("startup")
async def startup() -> None:
    bridge.set_loop(asyncio.get_running_loop())


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(STATIC / "index.html")


@app.get("/api/ports")
async def list_ports() -> dict[str, list[dict[str, str]]]:
    ports = []
    for p in serial.tools.list_ports.comports():
        ports.append({"device": p.device, "description": p.description or p.device})
    return {"ports": ports}


@app.get("/api/connection")
async def connection_status() -> dict[str, Any]:
    return {"connected": bridge.connected, "port": bridge.port, "baud": BAUD}


@app.post("/api/connect")
async def connect(body: dict[str, str]) -> dict[str, Any]:
    port = body.get("port", "").strip()
    if not port:
        raise HTTPException(400, "port required")
    try:
        bridge.connect(port)
    except Exception as exc:
        raise HTTPException(500, str(exc)) from exc
    return {"ok": True, "port": port}


@app.post("/api/disconnect")
async def disconnect() -> dict[str, bool]:
    bridge.disconnect()
    return {"ok": True}


@app.post("/api/shutdown")
async def shutdown_app() -> dict[str, str]:
    """Stop this PC app only — closes serial and exits the web server."""
    bridge.disconnect()

    async def _stop() -> None:
        await asyncio.sleep(0.25)
        os.kill(os.getpid(), signal.SIGINT)

    asyncio.create_task(_stop())
    return {"ok": True, "message": "Host Console closing…"}


@app.post("/api/command")
async def command(body: dict[str, str]) -> dict[str, Any]:
    line = body.get("line", "").strip()
    if not line:
        raise HTTPException(400, "line required")
    try:
        bridge.send_line(line)
    except RuntimeError as exc:
        raise HTTPException(409, str(exc)) from exc
    except Exception as exc:
        raise HTTPException(500, str(exc)) from exc
    return {"ok": True, "line": line}


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket) -> None:
    await ws.accept()
    await bridge.add_client(ws)
    try:
        while True:
            await ws.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        await bridge.remove_client(ws)


app.mount("/static", StaticFiles(directory=STATIC), name="static")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--http-port", type=int, default=DEFAULT_HTTP)
    ap.add_argument("--port", "-p", help="Auto-connect this serial device on start")
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()

    if args.port:
        threading.Timer(1.0, lambda: bridge.connect(args.port)).start()

    url = f"http://{args.host}:{args.http_port}/"
    if not args.no_browser:
        threading.Timer(1.2, lambda: webbrowser.open(url)).start()

    print(f"UL212 Host Console → {url}")
    print("Connect a host ESP32 over USB, pick the port in the browser.")
    uvicorn.run(app, host=args.host, port=args.http_port, log_level="warning")


if __name__ == "__main__":
    main()
