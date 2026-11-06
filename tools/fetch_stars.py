#!/usr/bin/env python3
"""Fetch the Tab5 star list (stars.csv) over the USB serial console.

The firmware command `@` dumps the current library's stars.csv using the
IAP2FILE base64 protocol. Example:

    python3 tools/fetch_stars.py /dev/ttyACM0 stars.csv
"""
from __future__ import annotations

import argparse
import base64
import re
import sys
import time
from pathlib import Path

import serial


def fetch(port: str, timeout: float) -> tuple[str, bytes]:
    deadline = time.time() + timeout
    with serial.Serial(port, 115200, timeout=0.2) as ser:
        ser.reset_input_buffer()
        ser.write(b"@")

        buf = bytearray()
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf.extend(chunk)
                if b"IAP2FILE_END" in buf:
                    break

    text = bytes(buf).decode("utf-8", "replace")
    match = re.search(
        r"IAP2FILE (?P<path>\S+) (?P<size>\d+)\r?\n"
        r"(?P<body>.*?)"
        r"IAP2FILE_END",
        text,
        re.S,
    )
    if not match:
        raise RuntimeError("no IAP2FILE block in the serial output")

    payload = re.sub(r"\s+", "", match.group("body"))
    data = base64.b64decode(payload)
    declared = int(match.group("size"))
    if declared != len(data):
        raise RuntimeError(
            f"size mismatch: firmware said {declared}, decoded {len(data)}"
        )
    return match.group("path"), data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?", default="/dev/ttyACM0")
    parser.add_argument("output", nargs="?", default="stars.csv")
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    try:
        remote, data = fetch(args.port, args.timeout)
    except Exception as exc:  # noqa: BLE001 - CLI should print a clean error
        print(f"error: {exc}", file=sys.stderr)
        return 1

    out = Path(args.output)
    out.write_bytes(data)
    print(f"{remote} -> {out} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
