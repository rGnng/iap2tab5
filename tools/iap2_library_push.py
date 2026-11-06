#!/usr/bin/env python3
"""Push a generated library.json to the Tab5 over the USB serial console.

The firmware command ``L`` waits for base64 lines and writes
``/sdcard/library.json``. Reboot the Tab5 afterwards to load it.
"""
from __future__ import annotations

import base64
import sys
import time

import serial


def wait_for(port: serial.Serial, needle: bytes, timeout: float) -> bytes:
    buf = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = port.read(65536)
        if chunk:
            buf += chunk
            if needle in buf:
                return buf
    raise TimeoutError(f"timed out waiting for {needle!r}; tail={buf[-200:]!r}")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: iap2_library_push.py file [port] [Q|B]")
        return 2
    path = sys.argv[1]
    dev = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0"
    cmd = sys.argv[3] if len(sys.argv) > 3 else "Q"

    payload = base64.b64encode(open(path, "rb").read())
    port = serial.Serial(dev, 115200, timeout=0.5)
    port.reset_input_buffer()
    port.write(cmd.encode())
    port.flush()
    wait_for(port, b"IAP2UP_READY", 10)

    line = 76
    for i in range(0, len(payload), line):
        port.write(payload[i:i + line] + b"\n")
        if (i // line) % 128 == 0:
            port.flush()
            time.sleep(0.001)
    port.write(b"EOF\n")
    port.flush()

    out = wait_for(port, b"IAP2UP_OK", 180)
    tail = out[-200:].decode("utf-8", "replace")
    print(tail)
    port.close()
    return 0 if b"IAP2UP_OK" in out else 1


if __name__ == "__main__":
    raise SystemExit(main())
