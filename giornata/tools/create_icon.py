"""Render the Giornata icon (the 24-hour ring with coloured events and the 'now' dot) as a 192px PNG.

    python giornata/tools/create_icon.py external_apps/giornata/icon.png
"""
import argparse
import math
from pathlib import Path
import struct
import zlib

BG = (10, 12, 20)
TRACK = (38, 44, 64)
PAST = (60, 68, 98)
EVENTS = [  # (from hour, to hour, colour)
    (8.5, 10.0, (47, 207, 127)),
    (11.0, 12.5, (255, 107, 154)),
    (13.0, 14.5, (255, 181, 71)),
    (15.5, 18.0, (124, 140, 255)),
    (20.0, 21.5, (79, 179, 255)),
]
NOW = 11.6


def colour_at(x, y):
    r = math.hypot(x, y)
    # angle from the top, clockwise, in hours
    hours = (math.degrees(math.atan2(x, -y)) % 360) / 15
    c = list(BG)
    glow = math.exp(-((r - 0.62) / 0.35) ** 2) * 0.25
    c = [v + (124 - v) * glow * 0.3 for v in c]
    if 0.56 <= r <= 0.80:
        c = list(TRACK if hours > NOW else PAST)
        for start, end, colour in EVENTS:
            if start <= hours <= end:
                c = list(colour)
                if end < NOW:
                    c = [v * 0.55 + BG[i] * 0.45 for i, v in enumerate(c)]
    # now dot on the ring
    a = math.radians(NOW * 15)
    nx, ny = math.sin(a) * 0.68, -math.cos(a) * 0.68
    d = math.hypot(x - nx, y - ny)
    if d < 0.15:
        c = [250, 252, 255]
    elif d < 0.21:
        k = (0.21 - d) / 0.06
        c = [v + (255 - v) * k * 0.6 for v in c]
    # centre: a check mark (two thick segments)
    def segment(px, py, ax, ay, bx, by):
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
        return math.hypot(px - ax - t * dx, py - ay - t * dy)
    if min(segment(x, y, -0.20, 0.0, -0.06, 0.14), segment(x, y, -0.06, 0.14, 0.20, -0.14)) < 0.05:
        c = [47, 207, 127]
    return c


def render():
    side, ss = 192, 3
    pixels = bytearray()
    for row in range(side):
        pixels.append(0)
        for col in range(side):
            acc = [0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    x = (col + (sx + 0.5) / ss - 96) / 96
                    y = (row + (sy + 0.5) / ss - 96) / 96
                    for i, v in enumerate(colour_at(x, y)):
                        acc[i] += v
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
    print(f"Giornata icon: {args.output}")
