#!/usr/bin/env python3
"""Download the Propeller P1 ROM and convert it for the emulator.

The P1 ROM ($8000-$FFFF: character font, log/antilog/sine tables, Spin
interpreter, boot loader) is part of Parallax's Propeller 1 Design,
released in 2014 under the GNU GPL v3.  This script takes it from the
P1V repository (pinned commit) as the two Intel HEX files used by its
FPGA build, and writes emu/rom/p1rom.bin: 32 KB, little-endian longs, as
seen from hub address $8000.  The interpreter and the boot loader stay
scrambled, as in the chip: the emulator unscrambles them when a cog loads
them, like the hub does (hub.v).

emu/rom/p1rom.bin is committed (see emu/rom/README.md for its origin);
run this script to check it or to rebuild it.

Usage: emu/tools/get_p1_rom.py
"""
import hashlib
import os
import struct
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMMIT = "feada86ade7a5a0dcabaf90a990483c243f7f4f9"
BASE = f"https://raw.githubusercontent.com/MicroNut/P1V/{COMMIT}/HDL/"
SHA256 = "f83da048d8e1f73fc04657ff6087a16200cb27d5cfae47a96a093294f0262488"


def load_hex(text):
    """Intel HEX with one 32-bit word per address unit -> 4096 words."""
    words = {}
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue
        n = int(line[1:3], 16)
        addr = int(line[3:7], 16)
        if int(line[7:9], 16) != 0:
            continue
        data = bytes.fromhex(line[9:9 + 2 * n])
        for k in range(n // 4):
            words[addr + k] = int.from_bytes(data[4 * k:4 * k + 4], "big")
    return [words.get(i, 0) for i in range(4096)]


def main():
    halves = []
    for name in ("hub_rom_low.hex", "hub_rom_high.hex"):      # $8000, $C000
        print("downloading", BASE + name)
        with urllib.request.urlopen(BASE + name) as r:
            halves.append(load_hex(r.read().decode("ascii")))
    image = b"".join(struct.pack("<I", w) for w in halves[0] + halves[1])
    digest = hashlib.sha256(image).hexdigest()
    if digest != SHA256:
        sys.exit(f"unexpected ROM contents (sha256 {digest})")
    out = os.path.join(ROOT, "rom", "p1rom.bin")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(image)
    print("written", out, "(Parallax Propeller 1 Design, GPL v3)")


if __name__ == "__main__":
    main()
