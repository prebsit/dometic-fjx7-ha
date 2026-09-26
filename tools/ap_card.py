#!/usr/bin/env python3
"""Print the setup-card details for a bridge built with packages/provisioning.yaml.

Usage:
    AP_KEY=<ap_key> python3 tools/ap_card.py <base MAC> [--name fjx7-bridge]

<base MAC> is the "MAC:" line esptool prints when flashing, e.g. 3c:84:27:ab:cd:ef.

Prints the hotspot SSID, password, and the WiFi QR string. Make the QR with:
    qrencode -t ANSIUTF8 "<WIFI:... string>"      # in the terminal
    qrencode -o card.png -s 10 "<WIFI:... string>" # as an image

Must stay byte-for-byte in step with the on_boot lambda in provisioning.yaml.
"""

import argparse
import hashlib
import hmac
import os
import sys

ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789"  # 31 symbols, no 0/o/1/l/i
DOMAIN = b"fjx7-ap-v1"


def parse_mac(text: str) -> bytes:
    hexdigits = text.replace(":", "").replace("-", "").strip().lower()
    if len(hexdigits) != 12:
        raise ValueError(f"expected 6-byte MAC, got {text!r}")
    return bytes.fromhex(hexdigits)


def ap_password(key: str, mac: bytes) -> str:
    d = hmac.new(key.encode(), DOMAIN + mac, hashlib.sha256).digest()
    chars = [ALPHABET[b % len(ALPHABET)] for b in d[:12]]
    return "".join(chars[0:4]) + "-" + "".join(chars[4:8]) + "-" + "".join(chars[8:12])


def ap_ssid(name: str, mac: bytes) -> str:
    # ESPHome name_add_mac_suffix: "<name>-" + last 3 MAC bytes, lowercase hex.
    # (Only truncated if longer than 32 chars; keep names short.)
    ssid = f"{name}-{mac[3:].hex()}"
    if len(ssid) > 32:
        sys.exit(f"SSID {ssid!r} is over 32 chars; ESPHome would truncate it. Use a shorter name.")
    return ssid


def qr_escape(s: str) -> str:
    for ch in '\\;,:"':
        s = s.replace(ch, "\\" + ch)
    return s


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mac", help="base MAC from esptool, e.g. 3c:84:27:ab:cd:ef")
    p.add_argument("--name", default="fjx7-bridge", help="ESPHome name substitution (default fjx7-bridge)")
    args = p.parse_args()

    key = os.environ.get("AP_KEY")
    if not key:
        sys.exit("Set AP_KEY to the same ap_key the firmware was built with.")

    mac = parse_mac(args.mac)
    ssid = ap_ssid(args.name, mac)
    pw = ap_password(key, mac)
    print(f"SSID:     {ssid}")
    print(f"Password: {pw}")
    print(f"QR:       WIFI:T:WPA;S:{qr_escape(ssid)};P:{qr_escape(pw)};;")


if __name__ == "__main__":
    main()
