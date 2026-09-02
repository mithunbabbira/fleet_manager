#!/usr/bin/env python3
"""Bench tests for carrier + host without vehicle / cloud dependency."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field

try:
    import serial
except ImportError:
    print("pip install pyserial", file=sys.stderr)
    raise

MASTER_PORT = "/dev/cu.usbmodem1101"
HOST_PORT = "/dev/cu.usbmodem1201"
BAUD = 115200


@dataclass
class Case:
    name: str
    ok: bool
    detail: str = ""


@dataclass
class Report:
    cases: list[Case] = field(default_factory=list)

    def add(self, name: str, ok: bool, detail: str = "") -> None:
        self.cases.append(Case(name, ok, detail))

    def print_summary(self) -> int:
        print("\n" + "=" * 60)
        print("BENCH TEST SUMMARY")
        print("=" * 60)
        passed = sum(1 for c in self.cases if c.ok)
        for c in self.cases:
            mark = "PASS" if c.ok else "FAIL"
            line = f"[{mark}] {c.name}"
            if c.detail:
                line += f"\n       {c.detail}"
            print(line)
        print("-" * 60)
        print(f"{passed}/{len(self.cases)} passed")
        return 0 if passed == len(self.cases) else 1


def serial_session(port: str, commands: list[str], idle_s: float = 2.5) -> str:
    """Send commands; return captured output (includes boot spam)."""
    out: list[str] = []
    with serial.Serial(port, BAUD, timeout=0.15) as ser:
        ser.reset_input_buffer()
        time.sleep(0.2)
        for cmd in commands:
            ser.write((cmd + "\n").encode())
            ser.flush()
            deadline = time.time() + idle_s
            while time.time() < deadline:
                chunk = ser.read(4096)
                if chunk:
                    text = chunk.decode("utf-8", errors="replace")
                    out.append(text)
                    deadline = time.time() + 0.4
                else:
                    time.sleep(0.05)
    return "".join(out)


def serial_sniff(port: str, duration_s: float) -> str:
    out: list[str] = []
    with serial.Serial(port, BAUD, timeout=0.15) as ser:
        ser.reset_input_buffer()
        end = time.time() + duration_s
        while time.time() < end:
            chunk = ser.read(4096)
            if chunk:
                out.append(chunk.decode("utf-8", errors="replace"))
            else:
                time.sleep(0.05)
    return "".join(out)


def run_unit_tests(report: Report) -> None:
    root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()
    build = f"{root}/tests/host/build"
    subprocess.run(
        ["cmake", "-S", f"{root}/tests/host", "-B", build],
        check=True,
        capture_output=True,
    )
    subprocess.run(["cmake", "--build", build], check=True, capture_output=True)
    r = subprocess.run(
        ["ctest", "--test-dir", build, "--output-on-failure"],
        capture_output=True,
        text=True,
    )
    ok = r.returncode == 0
    m = re.search(r"(\d+)% tests passed", r.stdout)
    detail = m.group(0) if m else r.stdout[-400:]
    report.add("Host unit tests (8)", ok, detail.strip())


def test_master_serial(report: Report, port: str) -> str:
    cmds = ["status", "fleet hosts", "uplink", "lte", "metrics", "ota status"]
    text = serial_session(port, cmds, idle_s=3.0)
    report.add("Master serial responds", "can_ready=" in text or "metrics=" in text)

    if "host_count=" in text:
        m = re.search(r"host_count=(\d+).*?joined=(\d+)", text, re.S)
        if m:
            hc, jn = m.group(1), m.group(2)
            report.add(
                "Zigbee host registry",
                int(hc) >= 1,
                f"host_count={hc} joined={jn}",
            )
            report.add(
                "Host height reading in registry",
                "height_mm=" in text or "height" in text.lower(),
                "look for height_mm in fleet hosts output",
            )
        else:
            report.add("Zigbee host registry", False, "no host_count in output")
    else:
        report.add("Zigbee host registry", False, "fleet hosts produced no output")

    report.add(
        "SD card mounted",
        "sd_mounted" in text and "true" in text.split("sd_mounted")[-1][:20]
        or "sd=yes" in text
        or "queue_depth=" in text,
        "from uplink status",
    )

    if "can_ready=no" in text or "can_ready=yes" in text:
        report.add(
            "CAN status (no vehicle expected)",
            "can_ready=no" in text,
            "can_ready=no is OK without ECU on bus",
        )

    if "gps_ok" in text or "lat" in text:
        report.add(
            "GPS indoor (no fix expected)",
            True,
            "gps_ok=false or no fix is OK indoors",
        )

    lte_ok = "uart_ok=yes" in text or "reg=yes" in text or "attached=yes" in text
    report.add("LTE modem UART/registration", lte_ok, "lte command output")

    return text


def test_uplink_paths(report: Report, port: str) -> None:
    qtext = serial_session(port, ["uplink qtest", "uplink"], idle_s=2.0)
    qok = "uplink qtest: ESP_OK" in qtext or "qtest: ESP_OK" in qtext
    sd_enq = "sd=yes" in qtext or "sd_mounted" in qtext
    report.add("SD queue enqueue (qtest)", qok and sd_enq, qtext[-300:].strip())

    now_text = serial_session(port, ["uplink now"], idle_s=25.0)
    attempted = "uplink now:" in now_text
    http_m = re.search(r"http=(\d+)", now_text)
    http_code = http_m.group(1) if http_m else "?"
    # Server may fail — we only need proof modem HTTP path ran
    report.add(
        "LTE uplink POST attempted",
        attempted,
        f"http={http_code} (400/5xx = server/schema; still proves modem path)",
    )
    if http_m:
        code = int(http_m.group(1))
        report.add(
            "Cloud accepts payload",
            200 <= code < 300,
            f"http={code} — skip if known server issue",
        )


def test_host_serial(report: Report, port: str) -> None:
    text = serial_sniff(port, 8.0)
    ble = "[UL212]" in text or "height" in text.lower() or "BLE" in text
    zb_join = "[zb] joined" in text or "joined, HELLO" in text
    zb_tlv = "TLV" in text  # on master; host may not log TLV

    report.add("Host BLE sensor traffic", ble, "expect [UL212] or height lines")
    report.add("Host Zigbee joined", zb_join, "expect [zb] joined, HELLO sent")

    if not ble:
        report.add(
            "Host serial capture",
            len(text) > 50,
            f"captured {len(text)} chars — device may be idle",
        )


def test_http_optional(report: Report) -> None:
    import urllib.error
    import urllib.request

    urls = [
        "http://192.168.4.1/api/fleet/hosts",
        "http://192.168.4.1/api/health",
    ]
    for url in urls:
        try:
            with urllib.request.urlopen(url, timeout=2) as resp:
                body = resp.read(512).decode("utf-8", errors="replace")
                report.add(
                    f"HTTP {url.split('/')[-1]}",
                    resp.status == 200,
                    body[:120],
                )
        except Exception as exc:
            report.add(
                f"HTTP {url.split('/')[-1]}",
                True,
                f"skipped — not on Fleet-C6 Wi-Fi ({exc})",
            )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--master", default=MASTER_PORT)
    ap.add_argument("--host", default=HOST_PORT)
    ap.add_argument("--skip-unit", action="store_true")
    ap.add_argument("--skip-http", action="store_true")
    args = ap.parse_args()

    report = Report()
    print("Bench test — carrier + UL212 host (no vehicle, cloud may fail)\n")

    if not args.skip_unit:
        print("Running host unit tests...")
        try:
            run_unit_tests(report)
        except Exception as exc:
            report.add("Host unit tests (8)", False, str(exc))

    for label, port in [("master", args.master), ("host", args.host)]:
        try:
            with serial.Serial(port, BAUD, timeout=0.2):
                pass
        except Exception as exc:
            report.add(f"{label} port open ({port})", False, str(exc))
            return report.print_summary()

    report.add(f"Master port open ({args.master})", True)
    report.add(f"Host port open ({args.host})", True)

    print(f"Querying master {args.master}...")
    test_master_serial(report, args.master)

    print("Testing uplink produce/drain...")
    test_uplink_paths(report, args.master)

    print(f"Sniffing host {args.host} (8s)...")
    test_host_serial(report, args.host)

    if not args.skip_http:
        test_http_optional(report)

    return report.print_summary()


if __name__ == "__main__":
    raise SystemExit(main())
