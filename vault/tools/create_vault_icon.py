"""Render the Vault icon (round safe door, gold keyhole) as a dependency-free 192px PNG.

    python vault/tools/create_vault_icon.py external_apps/vault/icon.png
"""
import argparse
import math
from pathlib import Path
import struct
import zlib

GOLD = (230, 188, 115)


def render():
    side = 192
    pixels = bytearray()
    for y in range(side):
        pixels.append(0)
        for x in range(side):
            px, py = (x - 95.5) / 96, (y - 95.5) / 96
            radius = math.hypot(px, py)
            # Night background with a warm halo behind the door.
            base = [9 + 10 * (1 - radius), 13 + 10 * (1 - radius), 21 + 16 * (1 - radius)]
            glow = math.exp(-((radius - .66) / .16) ** 2)
            base = [base[i] + glow * [46, 36, 22][i] for i in range(3)]
            if radius < .62:
                # Door: vertical gradient like the app's entrance screen.
                t = (py + .62) / 1.24
                base = [53 - 39 * t, 67 - 46 * t, 85 - 52 * t]
                light = max(0, 1 - math.hypot(px + .2, py + .25) / .9)
                base = [base[i] + 22 * light for i in range(3)]
            if abs(radius - .62) < .035:
                base = list(GOLD)
            # Bolt marks on the inner ring.
            if abs(radius - .5) < .02 and math.cos(8 * math.atan2(py, px)) > .92:
                base = [GOLD[i] * .7 for i in range(3)]
            # Keyhole: circle plus tapered stem.
            if math.hypot(px, py + .1) < .13 or (-.02 < py < .27 and abs(px) < .045 + .05 * (py + .02)):
                base = list(GOLD)
            pixels.extend(max(0, min(255, round(c))) for c in base)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(pixels), 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_bytes(render())
    print(f"Vault icon: {args.output}")
