#!/usr/bin/env python3
"""Generate firmware/main/iap2_identity.c from an MFi P-256 identity.

The identity files are intentionally NOT committed: the private key must stay
local. Point the script at the certificate and PKCS#8 key exported from the
MFi identity (for example `../iap2cert/certificate.p7b` and
`../iap2cert/identity.pk8`).

    python3 tools/gen_iap2_identity.py

The firmware build needs the generated file. It is listed in .gitignore.
"""
from __future__ import annotations

import argparse
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent.parent


def emit_array(name: str, data: bytes) -> str:
    lines = [f"const uint8_t {name}[] = {{"]
    for offset in range(0, len(data), 12):
        chunk = data[offset:offset + 12]
        body = ", ".join(f"0x{byte:02x}" for byte in chunk)
        lines.append(f"    {body},")
    lines.append("};")
    lines.append(f"const size_t {name}_len = sizeof({name});")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cert",
        type=Path,
        default=BASE_DIR.parent / "iap2cert" / "certificate.p7b",
        help="Apple certificate chain (PKCS#7 SignedData)",
    )
    parser.add_argument(
        "--key",
        type=Path,
        default=BASE_DIR.parent / "iap2cert" / "identity.pk8",
        help="MFi P-256 private key (PKCS#8 DER)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=BASE_DIR / "firmware" / "main" / "iap2_identity.c",
        help="generated C file",
    )
    args = parser.parse_args()

    if not args.cert.is_file() or not args.key.is_file():
        parser.error(
            "identity files not found; pass --cert and --key "
            "(the private key is not in the repository)"
        )

    cert = args.cert.read_bytes()
    key = args.key.read_bytes()
    text = (
        '#include "iap2_identity.h"\n\n'
        + emit_array("iap2_certificate_p7b", cert)
        + "\n\n"
        + emit_array("iap2_identity_sec1", key)
        + "\n"
    )
    args.out.write_text(text, encoding="ascii")
    print(
        f"wrote {args.out} (certificate {len(cert)} bytes, "
        f"key {len(key)} bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
