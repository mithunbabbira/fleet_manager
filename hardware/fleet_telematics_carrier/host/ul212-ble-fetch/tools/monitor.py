#!/usr/bin/env python3
"""Live BLE stability monitor over USB serial (parses [UL212] lines).

    python3 tools/monitor.py --port /dev/cu.usbmodem1201
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from collections import deque
from datetime import datetime

try:
    import serial
except ImportError:
    print("pip install pyserial", file=sys.stderr)
    raise

UL212_RE = re.compile(
    r"\[UL212\]\s+([\d.]+)\s+mm.*?signal\s+(\d+).*?([\d.]+)\s+C",
    re.I,
)


def fmt(seconds: float) -> str:
    s = int(seconds)
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m{s % 60:02d}s"
    return f"{s // 3600}h{(s % 3600) // 60:02d}m"


class Tracker:
    def __init__(self) -> None:
        self.sessions: list[float] = []
        self.outages: list[float] = []
        self.state: str | None = None
        self.since = time.time()
        self.samples = 0
        self.up_samples = 0

    def update(self, up: bool) -> str | None:
        now = time.time()
        self.samples += 1
        if up:
            self.up_samples += 1
        new = "up" if up else "down"
        if self.state is None:
            self.state, self.since = new, now
            return None
        if new == self.state:
            return None
        duration = now - self.since
        if self.state == "up":
            self.sessions.append(duration)
            event = f"LINK LOST after {fmt(duration)} of streaming"
        else:
            self.outages.append(duration)
            event = f"RECOVERED after {fmt(duration)} down"
        self.state, self.since = new, now
        return event

    @property
    def current(self) -> float:
        return time.time() - self.since


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", "-p", required=True)
    ap.add_argument("--stale-s", type=float, default=6.0, help="No UL212 line → down")
    args = ap.parse_args()

    t = Tracker()
    log: deque[str] = deque(maxlen=12)
    started = time.time()
    last_reading = 0.0
    last_height = None
    last_signal = None

    with serial.Serial(args.port, 115200, timeout=0.2) as ser:
        ser.reset_input_buffer()
        print(f"monitoring {args.port} — Ctrl-C for summary\n")
        try:
            while True:
                chunk = ser.read(4096).decode("utf-8", errors="replace")
                if chunk:
                    for m in UL212_RE.finditer(chunk):
                        last_reading = time.time()
                        last_height = float(m.group(1))
                        last_signal = int(m.group(2))

                up = (time.time() - last_reading) < args.stale_s
                event = t.update(up)
                if event:
                    log.append(f"{datetime.now():%H:%M:%S}  {event}")

                state = "STREAMING" if up else "DOWN     "
                line = (
                    f"{state} for {fmt(t.current):>7}  "
                    f"| {last_height if last_height is not None else '--'} mm  "
                    f"| signal {last_signal if last_signal is not None else '--'}"
                )

                print("\033[2J\033[H", end="")
                print(f"UL212 serial monitor — {args.port}")
                print(f"watching for {fmt(time.time() - started)}\n")
                if t.samples:
                    print(f"time streaming   : {100.0 * t.up_samples / t.samples:.1f}%")
                if log:
                    print("\nrecent events")
                    for e in log:
                        print("  " + e)
                print("\n" + line)
                time.sleep(1.0)
        except KeyboardInterrupt:
            pass

    print("\n\n=============== stability summary ===============")
    print(f"watched            : {fmt(time.time() - started)}")
    print(f"time streaming     : "
          f"{100.0 * t.up_samples / t.samples if t.samples else 0:.1f}%")
    print(f"good runs          : {len(t.sessions)}")
    if t.sessions:
        print(f"  longest          : {fmt(max(t.sessions))}")
        print(f"  average          : {fmt(sum(t.sessions) / len(t.sessions))}")
    print(f"outages            : {len(t.outages)}")
    print("=================================================")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
