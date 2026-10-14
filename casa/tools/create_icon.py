"""Render the Casa icon (a dark house with a warm glowing window and a bulb's halo) as a 192px PNG.

    python casa/tools/create_icon.py external_apps/casa/icon.png
"""
import argparse
import math
from pathlib import Path
import struct
import zlib

WARM = (255, 200, 112)


def inside_roof(px, py):
    """Triangle roof: apex (0, -0.62), base from (-0.62, -0.08) to (0.62, -0.08)."""
    if py < -0.62 or py > -0.08:
        return False
    half = (py + 0.62) / 0.54 * 0.62
    return abs(px) <= half


def render():
    side = 192
    pixels = bytearray()
    for row in range(side):
        pixels.append(0)
        for col in range(side):
            px, py = (col - 95.5) / 96, (row - 95.5) / 96
            # Night background with a warm halo where the window is.
            halo = math.exp(-((px / .55) ** 2 + ((py - .18) / .45) ** 2))
            color = [10 + 40 * halo, 12 + 28 * halo, 16 + 12 * halo]
            house = inside_roof(px, py) or (abs(px) <= .48 and -.1 <= py <= .62)
            if house:
                color = [27, 31, 41]
                if inside_roof(px, py) and not inside_roof(px * 1.1, py + .05):
                    color = [46, 52, 66]  # roof edge
            window = abs(px - .0) <= .17 and .08 <= py <= .4
            if window:
                glow = 1 - abs(py - .24) / .3
                color = [WARM[i] * (.75 + .25 * glow) for i in range(3)]
            if abs(px) <= .175 and abs(py - .24) <= .012 or abs(px) <= .012 and .08 <= py <= .4:
                color = [70, 52, 30]  # window cross
            pixels.extend(max(0, min(255, round(c))) for c in color)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(pixels), 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_bytes(render())
    print(f"Casa icon: {args.output}")
