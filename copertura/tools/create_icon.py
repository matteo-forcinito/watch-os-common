"""Render the Copertura icon (Wi-Fi waves over a heatmap blob, green to red) as a 192px PNG.

    python copertura/tools/create_icon.py external_apps/copertura/icon.png
"""
import argparse
import math
from pathlib import Path
import struct
import zlib

BG = (6, 11, 18)
HEAT = [(0.0, (46, 227, 154)), (0.45, (255, 209, 102)), (0.75, (255, 140, 60)), (1.0, (239, 68, 68))]


def heat_colour(t):
    for (a, ca), (b, cb) in zip(HEAT, HEAT[1:]):
        if t <= b:
            k = (t - a) / (b - a)
            return [ca[i] + (cb[i] - ca[i]) * k for i in range(3)]
    return list(HEAT[-1][1])


def colour_at(x, y):
    c = list(BG)
    # soft heatmap: hot near the bottom-right, cool at the top-left
    blobs = [(0.35, 0.35, 0.55, 0.0), (-0.35, -0.25, 0.45, 1.0), (0.25, -0.45, 0.4, 0.55)]
    wsum = vsum = 0.0
    for bx, by, r, v in blobs:
        w = math.exp(-((x - bx) ** 2 + (y - by) ** 2) / (r * r))
        wsum += w
        vsum += w * v
    if wsum > 0.05:
        h = heat_colour(vsum / wsum)
        a = min(1.0, wsum) * 0.55
        c = [c[i] + (h[i] - c[i]) * a for i in range(3)]
    # Wi-Fi symbol: dot + three arcs, centred low
    cx, cy = 0.0, 0.42
    r = math.hypot(x - cx, y - cy)
    ang = math.degrees(math.atan2(x - cx, -(y - cy)))
    white = [245, 250, 255]
    if r < 0.11:
        c = white
    for radius in (0.36, 0.6, 0.84):
        if abs(r - radius) < 0.065 and abs(ang) < 45:
            c = white
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
    print(f"Copertura icon: {args.output}")
