#!/usr/bin/env python3
"""Host-side AT probe for Quectel EC200U over USB (/dev/ttyUSB*).

For ESP↔modem UART wiring checks, flash firmware with CONFIG_NET_LTE_ENABLE
and watch serial for: net_lte: AT OK (wiring looks good)
"""

from __future__ import annotations

import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    print("Missing pyserial. Install with: sudo apt install python3-serial", file=sys.stderr)
    sys.exit(2)

CMDS = [
    "AT",
    "ATI",
    "AT+CGMM",
    "AT+CPIN?",
    "AT+CREG?",
    "AT+CEREG?",
    "AT+CGATT?",
    "AT+CGDCONT?",
]


def _is_printable_at_response(raw: bytes) -> bool:
    if not raw:
        return False
    text = raw.decode("ascii", errors="ignore")
    upper = text.upper()
    if "OK" not in upper and "ERROR" not in upper and "READY" not in upper:
        return False
    printable = sum(1 for b in raw if 32 <= b < 127 or b in (9, 10, 13))
    return (printable / len(raw)) >= 0.7


def probe_port(path: str, baud: int) -> bool:
    print(f"\n=== {path} @ {baud} ===")
    try:
        ser = serial.Serial(path, baud, timeout=0.5, write_timeout=1)
    except Exception as exc:  # noqa: BLE001
        print(f"open fail: {exc}")
        return False

    time.sleep(0.15)
    ser.reset_input_buffer()
    ser.write(b"AT\r")
    ser.flush()
    time.sleep(0.3)
    ser.reset_input_buffer()

    saw_at = False
    try:
        for cmd in CMDS:
            ser.write((cmd + "\r").encode("ascii"))
            ser.flush()
            time.sleep(0.5)
            raw = ser.read(4096)
            text = raw.decode("utf-8", errors="replace").strip()
            print(f"> {cmd}")
            print(text if text else "(no response)")
            if _is_printable_at_response(raw):
                saw_at = True
    finally:
        ser.close()
    return saw_at


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port")
    parser.add_argument("-b", "--baud", type=int, default=115200)
    parser.add_argument("--all-baud", action="store_true")
    args = parser.parse_args()

    ports = [args.port] if args.port else sorted(glob.glob("/dev/ttyUSB*"))
    if not ports:
        print("No /dev/ttyUSB* ports found.", file=sys.stderr)
        return 1

    bauds = [args.baud]
    if args.all_baud:
        for b in (9600, 115200, 921600):
            if b not in bauds:
                bauds.append(b)

    print("If ports busy: sudo systemctl stop ModemManager")

    found = None
    for path in ports:
        for baud in bauds:
            if probe_port(path, baud):
                found = f"{path} @ {baud}"
                break
        if found:
            break

    if found:
        print(f"\nLikely AT port: {found}")
        return 0
    print("No clean AT OK/ERROR.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
