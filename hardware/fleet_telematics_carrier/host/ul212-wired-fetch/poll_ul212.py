#!/usr/bin/env python3
"""Live UL212 wired reader (CH340 → MAX232 → sensor).

Protocol 14 (*XD) is GPS-style: often ~8–10 s unless the app interval is 1 s.
Protocol 51 (*CFV) answers every poll (~1 Hz) but height only.

  python3 poll_ul212.py /dev/cu.usbserial-110
  python3 poll_ul212.py --fast51 /dev/cu.usbserial-110
"""

import argparse
import glob
import sys
import time

import serial

CMD14 = b"$!RY0114\r\n"
CMD51 = b"$!RY0151\r\n"
BAUD = 9600
WAKE_S = 12.0


def find_port(explicit):
    if explicit:
        return explicit
    ports = sorted(
        glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wchusbserial*")
    )
    if not ports:
        raise SystemExit("No CH340 port. Plug it in or pass the device path.")
    return ports[0]


def parse_xd(text):
    if "*XD" not in text:
        return None
    body = text[text.find("*XD") :]
    body = body.split("#")[0]
    body = body.replace("*XD", "").replace(",", " ")
    p = body.split()
    if len(p) < 7:
        return None
    last = p[6]
    return {
        "smooth_mm": int(p[2]) * 0.1,
        "realtime_mm": int(p[4]) * 0.1,
        "signal": int(p[3][:2]) if len(p[3]) >= 2 else int(p[3]),
        "temp_c": (int(p[5]) - 400) * 0.1,
        "tilt_deg": int(last[-2:], 16) if len(last) >= 2 else 0,
        "raw": text.strip(),
    }


def parse_cfv(text):
    if "*CFV" not in text:
        return None
    body = text[text.find("*CFV") :].split("\r")[0].split("\n")[0]
    if len(body) < 12:
        return None
    raw = int(body[6:11])
    return {"height_mm": raw * 0.1, "raw": body}


def print_row(parsed):
    ts = time.strftime("%H:%M:%S")
    print(
        f"{ts}  height={parsed['realtime_mm']:7.1f} mm  "
        f"smooth={parsed['smooth_mm']:7.1f} mm  "
        f"temp={parsed['temp_c']:5.1f} C  "
        f"tilt={parsed['tilt_deg']:3d} deg  "
        f"signal={parsed['signal']:3d}"
    )


def txn(ser, cmd, wait):
    ser.reset_input_buffer()
    ser.write(cmd)
    ser.flush()
    buf = bytearray()
    t0 = time.time()
    last = t0
    while time.time() - t0 < wait:
        n = ser.in_waiting
        if n:
            buf.extend(ser.read(n))
            last = time.time()
            if b"\n" in buf or b"#" in buf:
                break
        elif buf and (time.time() - last) > 0.08:
            break
        else:
            time.sleep(0.005)
    return bytes(buf)


def run_fast51(ser):
    print("FAST height (~1 Hz). App must be protocol 51. No tilt/temp on this protocol.\n")
    while True:
        rx = txn(ser, CMD51, 0.4)
        ts = time.strftime("%H:%M:%S")
        parsed = parse_cfv(rx.decode("ascii", errors="replace")) if rx else None
        if parsed:
            print(f"{ts}  height={parsed['height_mm']:7.1f} mm")
        elif rx:
            print(f"{ts}  RAW {rx!r}")
        else:
            print(f"{ts}  (no reply)")
        time.sleep(0.2)


def run_listen14(ser):
    print("Protocol 14 listen (full fields). Sensor usually sends every ~8–10 s.\n")
    ser.reset_input_buffer()
    ser.write(CMD14)
    ser.flush()
    last_rx = time.time()
    buf = bytearray()
    while True:
        chunk = ser.read(256)
        if chunk:
            buf.extend(chunk)
            last_rx = time.time()
            while True:
                text = buf.decode("ascii", errors="replace")
                if "#" not in text and "\n" not in text:
                    break
                if "#" in text:
                    i = text.find("#") + 1
                else:
                    i = text.find("\n") + 1
                frame = text[:i]
                del buf[: len(frame.encode("ascii", errors="replace"))]
                parsed = parse_xd(frame)
                if parsed:
                    print_row(parsed)
                elif frame.strip():
                    print(f"{time.strftime('%H:%M:%S')}  RAW {frame.strip()!r}")
        else:
            if time.time() - last_rx >= WAKE_S:
                ser.write(CMD14)
                ser.flush()
                last_rx = time.time()
            time.sleep(0.02)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", help="e.g. /dev/cu.usbserial-110")
    ap.add_argument(
        "--fast51",
        action="store_true",
        help="1 Hz height only (set TankOffline protocol to 51)",
    )
    args = ap.parse_args()
    port = find_port(args.port)
    print(f"UL212  {port} @ {BAUD}   Ctrl+C to stop")
    ser = serial.Serial(port, BAUD, timeout=0.05, write_timeout=1)
    time.sleep(0.2)
    try:
        if args.fast51:
            run_fast51(ser)
        else:
            run_listen14(ser)
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
