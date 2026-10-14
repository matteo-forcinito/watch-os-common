"""Render the Pieno icon (a fuel drop with a price arrow going down) as a 192px PNG.

    python pieno/tools/create_icon.py external_apps/pieno/icon.png
"""
import argparse
import math
from pathlib import Path
import struct
import zlib

TOP = (46, 227, 154)      # green
BOTTOM = (14, 150, 120)   # teal
BG = (7, 16, 14)


def inside_drop(x, y):
    """Drop: a circle (centre 0,0.18 r 0.5) with a pointed top at (0,-0.68)."""
    cy, r = 0.18, 0.5
    if (x * x + (y - cy) ** 2) <= r * r:
        return True
    if y < cy and y > -0.68:
        # tangent lines from the tip to the circle
        half = (y + 0.68) / (cy + 0.68) * r * 1.02
        return abs(x) <= half
    return False


def inside_arrow(x, y):
    """White down arrow in the drop: a stem and a head."""
    stem = abs(x) <= 0.07 and -0.12 <= y <= 0.26
    head = 0.18 <= y <= 0.46 and abs(x) <= (0.46 - y) * 0.95
    return stem or head


def render():
    side, ss = 192, 3   # 3x3 supersampling for smooth edges
    pixels = bytearray()
    for row in range(side):
        pixels.append(0)
        for col in range(side):
            acc = [0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    x = (col + (sx + 0.5) / ss - 96) / 96
                    y = (row + (sy + 0.5) / ss - 96) / 96
                    glow = math.exp(-((x / 0.7) ** 2 + ((y - 0.1) / 0.7) ** 2)) * 0.35
                    c = [BG[i] + (TOP[i] - BG[i]) * glow * 0.4 for i in range(3)]
                    if inside_drop(x, y):
                        t = min(1, max(0, (y + 0.68) / 1.36))
                        c = [TOP[i] + (BOTTOM[i] - TOP[i]) * t for i in range(3)]
                        # soft highlight on the upper left
                        h = math.exp(-(((x + 0.2) / 0.18) ** 2 + ((y + 0.02) / 0.3) ** 2)) * 0.35
                        c = [v + (255 - v) * h for v in c]
                        if inside_arrow(x, y):
                            c = [250, 255, 252]
                    for i in range(3):
                        acc[i] += c[i]
            pixels.extend(max(0, min(255, round(v / (ss * ss)))) for v in acc)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(pixels), 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_bytes(render())
    print(f"Pieno icon: {args.output}")
