#!/usr/bin/env python3
"""Local web console for fleet carrier ESP32-C6 — serial bridge only.

    pip install -r tools/carrier_console/requirements.txt
    python3 tools/carrier_console/app.py
    python3 tools/carrier_console/app.py --port /dev/cu.usbmodem1101

Opens http://127.0.0.1:8766 — ESP32 stays serial-only; all UI runs on the PC.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import signal
import threading
import time
import webbrowser
from contextlib import asynccontextmanager
from copy import deepcopy
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
DEFAULT_HTTP = 8766

# USB Serial/JTAG identity (macOS exposes this as serial_number).
CARRIER_USB_SN = "10:BD:A3:96:5A:0C"
HOST_USB_SN = "58:E6:C5:DB:7B:D4"  # refuse — Host Console owns this device

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
HOST_COUNT_RE = re.compile(r"host_count=(\d+)\s+joined=(\d+)")
HOST_LINE_RE = re.compile(
    r"^\s{2}(\S+)\s+type=(\S+)\s+id=(\d+)(?:\s+node=(\S+)\s+schema=(\S+))?\s+link=(\d+)\s+last=(\d+)"
)
READING_RE = re.compile(r"^\s{4}(\S+)=(-?[\d.]+)(?:\s+(\S+))?")
METRICS_RE = re.compile(r"^metrics=(\{.*\})$")
CAN_RE = re.compile(r"^can_ready=(\w+) protocol=(\S+) poller=(\w+) profile=(\S+)")
OTA_FW_RE = re.compile(r"^ota:\s*fw=([^\s]+)")
PROVISION_RE = re.compile(r"^provisioned=(\w+)")


def strip_ansi(s: str) -> str:
    return ANSI_RE.sub("", s)


def port_serial_number(port: str) -> str | None:
    for p in serial.tools.list_ports.comports():
        if p.device == port:
            return p.serial_number
    return None


def assert_carrier_port(port: str) -> None:
    sn = port_serial_number(port)
    if sn == HOST_USB_SN:
        raise RuntimeError(
            f"{port} is the UL212 host (SN {HOST_USB_SN}). "
            "Use Host Console there; Carrier Console needs "
            f"SN {CARRIER_USB_SN} (usually /dev/cu.usbmodem1201)."
        )


def find_port_by_sn(serial_number: str) -> str | None:
    for p in serial.tools.list_ports.comports():
        if p.serial_number == serial_number:
            return p.device
    return None


class SerialBridge:
    """One open serial port; fan-out lines to WebSocket clients."""

    def __init__(self) -> None:
        self._ser: serial.Serial | None = None
        self._port: str | None = None
        self._clients: set[WebSocket] = set()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._reader: threading.Thread | None = None
        self._poller: threading.Thread | None = None
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._state_lock = threading.RLock()
        self._last_rx_at: float | None = None
        self._dashboard: dict[str, Any] = {
            "hosts": {"host_count": 0, "joined": 0, "hosts": [], "updated_at": None},
            "carrier": {
                "uptime_s": None,
                "zigbee_reports": None,
                "can_ready": None,
                "can_meta": None,
                "fw": None,
                "provisioned": None,
            },
        }
        self._fleet_snap: dict[str, Any] | None = None
        self._fleet_host: dict[str, Any] | None = None
        self._fleet_waiters: list[threading.Event] = []
        self._cmd_lock = threading.Lock()

    @property
    def connected(self) -> bool:
        return self._ser is not None and self._ser.is_open

    @property
    def last_rx_age_ms(self) -> int | None:
        if self._last_rx_at is None:
            return None
        return int((time.monotonic() - self._last_rx_at) * 1000)

    @property
    def port(self) -> str | None:
        return self._port

    def dashboard_snapshot(self) -> dict[str, Any]:
        with self._state_lock:
            return deepcopy(self._dashboard)

    def set_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop

    def connect(self, port: str) -> None:
        assert_carrier_port(port)
        self.disconnect()
        with self._lock:
            # Set DTR/RTS before open — post-open is too late on ESP32-C6 USB-JTAG
            # and can leave the ROM in "waiting for download".
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = BAUD
            ser.timeout = 0.15
            ser.dtr = False
            ser.rts = False
            ser.open()
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
            time.sleep(0.15)
            try:
                ser.reset_input_buffer()
            except Exception:
                pass
            self._ser = ser
            self._port = port
            self._last_rx_at = None
            self._stop.clear()
            self._reader = threading.Thread(target=self._read_loop, daemon=True)
            self._reader.start()
            self._poller = threading.Thread(target=self._poll_loop, daemon=True)
            self._poller.start()
        self._schedule_broadcast({"type": "clear_log"})
        self._schedule_broadcast(
            {
                "type": "system",
                "line": f"Connected to {port} @ {BAUD} (carrier SN {port_serial_number(port) or '?'})",
            }
        )

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
            self._last_rx_at = None
        with self._state_lock:
            self._fleet_snap = None
            self._fleet_host = None
        for ev in self._fleet_waiters:
            ev.set()
        self._fleet_waiters.clear()
        if old:
            self._schedule_broadcast({"type": "system", "line": f"Disconnected from {old}"})

    def _on_serial_lost(self, reason: str) -> None:
        with self._lock:
            if self._ser is None and self._port is None:
                return
            if self._ser and self._ser.is_open:
                try:
                    self._ser.close()
                except Exception:
                    pass
            port = self._port
            self._ser = None
            self._port = None
            self._last_rx_at = None
        self._stop.set()
        for ev in self._fleet_waiters:
            ev.set()
        self._fleet_waiters.clear()
        self._schedule_broadcast({"type": "serial_lost", "port": port, "reason": reason})
        if port:
            self._schedule_broadcast(
                {
                    "type": "system",
                    "line": (
                        f"Serial lost ({reason}). Not auto-reopening USB "
                        "(avoids ESP32-C6 download mode). Click Connect after the board is up; "
                        "press RESET on the ESP if you see 'waiting for download'."
                    ),
                }
            )

    def send_line(self, line: str) -> None:
        with self._lock:
            if not self._ser or not self._ser.is_open:
                raise RuntimeError("not connected")
            self._ser.write((line.strip() + "\n").encode())
            self._ser.flush()
        self._schedule_broadcast({"type": "tx", "line": line.strip()})

    def refresh_dashboard(self, timeout: float = 5.0) -> dict[str, Any]:
        """
        Send quick host updates (status + fleet hosts) frequently, and only run
        slow LTE/OTA/uplink commands on a slower cadence.

        This keeps Zigbee-fueled host cards responsive while still letting the
        UI monitor other carrier health over time.
        """
        if not self.connected:
            raise RuntimeError("not connected")

        # Heavy carrier queries (LTE/OTA/uplink/GNSS/SD reads) can take seconds.
        # If we run them every 5s, host card updates can appear delayed.
        now_mono = time.monotonic()
        slow_interval_s = 30.0
        last_slow = getattr(self, "_last_slow_refresh_monotonic", 0.0)
        do_slow = (now_mono - last_slow) >= slow_interval_s

        with self._cmd_lock:
            self.send_line("status")
            time.sleep(0.5)
            done = threading.Event()
            self._fleet_waiters.append(done)
            try:
                self.send_line("fleet hosts")
                done.wait(timeout)
            finally:
                if done in self._fleet_waiters:
                    self._fleet_waiters.remove(done)
            if self._fleet_snap is not None:
                self._finish_fleet_parse()

            # Slow cadence: carrier identity + LTE + uplink status.
            if do_slow:
                # Fill Carrier Console forms + KV (parse in UI / dashboard).
                self.send_line("config")
                time.sleep(0.2)
                self.send_line("ota status")
                time.sleep(0.2)
                self.send_line("lte")
                time.sleep(0.2)
                self.send_line("uplink")
                time.sleep(0.35)
                self._last_slow_refresh_monotonic = time.monotonic()
        snap = self.dashboard_snapshot()
        self._schedule_broadcast({"type": "dashboard", "data": snap})
        return snap

    def _poll_loop(self) -> None:
        while not self._stop.is_set():
            if self.connected:
                try:
                    self.refresh_dashboard()
                except Exception:
                    pass
            if self._stop.wait(5.0):
                break

    def _finish_fleet_parse(self) -> None:
        if self._fleet_snap is None:
            return
        self._dashboard["hosts"] = {
            "host_count": self._fleet_snap["host_count"],
            "joined": self._fleet_snap["joined"],
            "hosts": self._fleet_snap["hosts"],
            "updated_at": time.time(),
        }
        self._fleet_snap = None
        self._fleet_host = None
        for ev in self._fleet_waiters:
            ev.set()
        self._schedule_broadcast(
            {"type": "dashboard", "data": self.dashboard_snapshot()}
        )

    def _ingest_line(self, raw: str) -> None:
        raw_line = strip_ansi(raw).replace("\r", "")
        trimmed = raw_line.strip()
        if not trimmed:
            return

        with self._state_lock:
            m = HOST_COUNT_RE.search(trimmed)
            if m:
                self._finish_fleet_parse()
                self._fleet_snap = {
                    "host_count": int(m.group(1)),
                    "joined": int(m.group(2)),
                    "hosts": [],
                }
                self._fleet_host = None
                if self._fleet_snap["host_count"] == 0:
                    self._finish_fleet_parse()
                return

            if self._fleet_snap is not None:
                hm = HOST_LINE_RE.match(raw_line)
                if not hm and not raw_line.startswith("  "):
                    hm = HOST_LINE_RE.match("  " + trimmed)
                if hm:
                    self._fleet_host = {
                        "device_id": hm.group(1),
                        "host_type": hm.group(2),
                        "host_type_id": int(hm.group(3)),
                        "node_id": hm.group(4) if hm.group(4) else "",
                        "schema_id": hm.group(5) if hm.group(5) else "",
                        "link": hm.group(6),
                        "last": hm.group(7),
                        "readings": {},
                    }
                    self._fleet_snap["hosts"].append(self._fleet_host)
                    return

                rm = READING_RE.match(raw_line)
                if rm and self._fleet_host is not None:
                    self._fleet_host["readings"][rm.group(1)] = {
                        "value": rm.group(2),
                        "unit": rm.group(3) or "",
                    }
                    return

                if "obd>" in trimmed and self._fleet_snap is not None:
                    expected = self._fleet_snap["host_count"]
                    got = len(self._fleet_snap["hosts"])
                    if expected == 0 or got >= expected:
                        self._finish_fleet_parse()

            carrier = self._dashboard["carrier"]
            mm = METRICS_RE.match(trimmed)
            if mm:
                try:
                    metrics = json.loads(mm.group(1))
                    if "uptime_s" in metrics:
                        carrier["uptime_s"] = metrics["uptime_s"]
                    if "zigbee_reports" in metrics:
                        carrier["zigbee_reports"] = metrics["zigbee_reports"]
                except json.JSONDecodeError:
                    pass

            cm = CAN_RE.match(trimmed)
            if cm:
                carrier["can_ready"] = cm.group(1)
                carrier["can_meta"] = {
                    "protocol": cm.group(2),
                    "poller": cm.group(3),
                    "profile": cm.group(4),
                }

            om = OTA_FW_RE.match(trimmed)
            if om:
                carrier["fw"] = om.group(1)

            pm = PROVISION_RE.match(trimmed)
            if pm:
                carrier["provisioned"] = pm.group(1) == "yes"

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
                self._on_serial_lost(str(exc))
                return
            if not chunk:
                # Quiet periods are normal — do NOT treat idle USB as device loss.
                # (Old idle-timeout reopen cycles forced ESP32-C6 into download mode.)
                continue
            self._last_rx_at = time.monotonic()
            buf += chunk.decode("utf-8", errors="replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.rstrip("\r")
                if not line:
                    continue
                self._ingest_line(line)
                self._schedule_broadcast({"type": "rx", "line": line})
        if not self._stop.is_set():
            self._on_serial_lost("read loop ended")

    def _schedule_broadcast(self, msg: dict[str, Any]) -> None:
        if not self._loop:
            return
        asyncio.run_coroutine_threadsafe(self.broadcast(msg), self._loop)

    async def add_client(self, ws: WebSocket) -> None:
        self._clients.add(ws)
        await ws.send_text(
            json.dumps({"type": "dashboard", "data": self.dashboard_snapshot()})
        )

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


@asynccontextmanager
async def lifespan(app: FastAPI):
    bridge.set_loop(asyncio.get_running_loop())
    yield
    bridge.disconnect()


app = FastAPI(title="Fleet Carrier Console", docs_url=None, redoc_url=None, lifespan=lifespan)


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(STATIC / "index.html")


@app.get("/api/ports")
async def list_ports() -> dict[str, Any]:
    ports = []
    for p in serial.tools.list_ports.comports():
        ports.append(
            {
                "device": p.device,
                "description": p.description or p.device,
                "serial_number": p.serial_number or "",
            }
        )
    return {"ports": ports, "carrier_usb_sn": CARRIER_USB_SN, "host_usb_sn": HOST_USB_SN}


@app.get("/api/connection")
async def connection_status() -> dict[str, Any]:
    age = bridge.last_rx_age_ms
    return {
        "connected": bridge.connected,
        "port": bridge.port,
        "baud": BAUD,
        "last_rx_age_ms": age,
        "rx_stale": bridge.connected and age is not None and age > 12000,
    }


@app.get("/api/dashboard")
async def dashboard() -> dict[str, Any]:
    return bridge.dashboard_snapshot()


@app.post("/api/dashboard/refresh")
async def dashboard_refresh() -> dict[str, Any]:
    try:
        return await asyncio.to_thread(bridge.refresh_dashboard)
    except RuntimeError as exc:
        raise HTTPException(409, str(exc)) from exc
    except Exception as exc:
        raise HTTPException(500, str(exc)) from exc


@app.post("/api/connect")
async def connect(body: dict[str, str]) -> dict[str, Any]:
    port = body.get("port", "").strip()
    if not port:
        raise HTTPException(400, "port required")
    try:
        bridge.connect(port)
    except Exception as exc:
        raise HTTPException(500, str(exc)) from exc
    threading.Timer(2.0, lambda: bridge.refresh_dashboard()).start()
    return {"ok": True, "port": port}


@app.post("/api/disconnect")
async def disconnect() -> dict[str, bool]:
    bridge.disconnect()
    return {"ok": True}


@app.post("/api/shutdown")
async def shutdown_app() -> dict[str, str]:
    bridge.disconnect()

    async def _stop() -> None:
        await asyncio.sleep(0.25)
        os.kill(os.getpid(), signal.SIGINT)

    asyncio.create_task(_stop())
    return {"ok": True, "message": "Carrier Console closing…"}


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
    ap.add_argument(
        "--port",
        "-p",
        help=f"Auto-connect this serial device (must be carrier SN {CARRIER_USB_SN})",
    )
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()

    port = args.port
    if not port:
        port = find_port_by_sn(CARRIER_USB_SN)
    if port:
        def _auto() -> None:
            try:
                bridge.connect(port)
            except Exception as exc:
                print(f"Carrier auto-connect failed: {exc}")

        threading.Timer(3.0, _auto).start()
        print(f"Will auto-connect carrier port {port}")
    else:
        print(f"Carrier USB SN {CARRIER_USB_SN} not found — pick port in UI")

    url = f"http://{args.host}:{args.http_port}/"
    if not args.no_browser:
        threading.Timer(3.2, lambda: webbrowser.open(url)).start()

    print(f"Fleet Carrier Console → {url}")
    print("Connect the carrier ESP32 over USB, pick the port in the browser.")
    uvicorn.run(app, host=args.host, port=args.http_port, log_level="warning")


if __name__ == "__main__":
    main()
