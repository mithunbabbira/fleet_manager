#!/usr/bin/env python3
"""Provision a UL212 host over USB serial (no Wi-Fi).

Examples:
  python3 provision_serial.py --port /dev/cu.usbmodem1201 --scan
  python3 provision_serial.py --port /dev/cu.usbmodem1201 \\
      --mac 00:65:01:0A:54:B7 --id ul212-042
  python3 provision_serial.py --port /dev/cu.usbmodem1201 --interactive
"""

from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("pip install pyserial", file=sys.stderr)
    raise


BAUD = 115200


def open_port(path: str) -> serial.Serial:
    ser = serial.Serial(path, BAUD, timeout=0.2)
    time.sleep(0.3)
    ser.reset_input_buffer()
    return ser


def send_cmd(ser: serial.Serial, cmd: str, wait_s: float = 2.5) -> str:
    ser.write((cmd + "\n").encode())
    ser.flush()
    end = time.time() + wait_s
    chunks: list[str] = []
    while time.time() < end:
        data = ser.read(4096)
        if data:
            chunks.append(data.decode("utf-8", errors="replace"))
            end = time.time() + 0.4
        else:
            time.sleep(0.05)
    return "".join(chunks)


def provision(
    port: str,
    mac: str | None,
    device_id: str | None,
    poll_ms: int | None,
    silence_ms: int | None,
    do_scan: bool,
    do_status: bool,
) -> int:
    ser = open_port(port)
    print(f"Connected {port} @ {BAUD}")

    if do_scan:
        out = send_cmd(ser, "scan", wait_s=8.0)
        print(out.strip() or "(no scan output)")

    if mac:
        out = send_cmd(ser, f"mac {mac.upper()}")
        print(out.strip())

    if device_id:
        out = send_cmd(ser, f"id {device_id}")
        print(out.strip())

    if poll_ms is not None:
        out = send_cmd(ser, f"poll {poll_ms}")
        print(out.strip())

    if silence_ms is not None:
        out = send_cmd(ser, f"silence {silence_ms}")
        print(out.strip())

    if mac or device_id or poll_ms is not None or silence_ms is not None:
        print("Saving to NVS and rebooting host...")
        try:
            ser.write(b"save\n")
            ser.flush()
            time.sleep(2.0)
        except Exception:
            pass
        ser.close()
        time.sleep(3.0)
        ser = open_port(port)
        time.sleep(1.5)

    if do_status:
        out = send_cmd(ser, "status", wait_s=3.0)
        print(out.strip())

    ser.close()
    print("\nOn carrier USB serial: fleet hosts")
    return 0


def interactive(port: str) -> int:
    ser = open_port(port)
    print(f"Interactive serial on {port}. Type host commands (help, scan, save). Ctrl-C to exit.\n")
    try:
        while True:
            line = input("host> ").strip()
            if not line:
                continue
            if line in ("quit", "exit"):
                break
            wait = 8.0 if line == "scan" else 3.0
            out = send_cmd(ser, line, wait_s=wait)
            if out.strip():
                print(out.rstrip())
    except KeyboardInterrupt:
        print()
    finally:
        ser.close()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", "-p", required=True, help="USB serial device")
    ap.add_argument("--mac", help="Sensor BLE MAC AA:BB:CC:DD:EE:FF")
    ap.add_argument("--id", dest="device_id", help="Zigbee device_id e.g. ul212-042")
    ap.add_argument("--poll", type=int, help="Poll interval ms")
    ap.add_argument("--silence", type=int, help="Silence timeout ms")
    ap.add_argument("--scan", action="store_true", help="Run BLE scan only")
    ap.add_argument("--status", action="store_true", help="Print status after provisioning")
    ap.add_argument("--interactive", "-i", action="store_true", help="Interactive command shell")
    args = ap.parse_args()

    if args.interactive:
        return interactive(args.port)

    if not any([args.mac, args.device_id, args.poll, args.silence, args.scan, args.status]):
        ap.print_help()
        return 1

    return provision(
        args.port,
        args.mac,
        args.device_id,
        args.poll,
        args.silence,
        args.scan,
        args.status or bool(args.mac or args.device_id),
    )


if __name__ == "__main__":
    raise SystemExit(main())
