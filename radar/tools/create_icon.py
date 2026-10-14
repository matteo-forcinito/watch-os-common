"""Render the Radar icon (dark map, a rain cell drifting toward the home dot, scale ring) as a 192px PNG.

    python radar/tools/create_icon.py external_apps/radar/icon.png
"""
import argparse
import math
from pathlib import Path
import struct
import zlib


def rain(px, py):
    """Soft rain cell: two overlapping blobs, intensity 0..1."""
    a = math.exp(-(((px + .32) / .34) ** 2 + ((py + .18) / .22) ** 2))
    b = math.exp(-(((px + .05) / .22) ** 2 + ((py - .12) / .18) ** 2))
    return min(1.0, a * .9 + b * .8)


def render():
    side = 192
    pixels = bytearray()
    for row in range(side):
        pixels.append(0)
        for col in range(side):
            px, py = (col - 95.5) / 96, (row - 95.5) / 96
            # Dark map with faint "roads".
            v = 18 + 6 * (math.sin(px * 9 + py * 4) > .96) + 5 * (math.sin(px * 3 - py * 11) > .97)
            color = [v - 4, v - 1, v + 9]
            intensity = rain(px, py)
            if intensity > .15:
                t = (intensity - .15) / .85
                tint = [40 + 40 * t, 150 + 60 * t, 235 + 20 * t] if t < .7 else [120 + 100 * (t - .7) / .3, 210, 255]
                alpha = .55 + .4 * t
                color = [color[i] * (1 - alpha) + tint[i] * alpha for i in range(3)]
            radius = math.hypot(px - .22, py - .22)
            if abs(radius - .36) < .012:
                color = [c * .5 + 255 * .5 for c in color]
            if radius < .05:
                color = [255, 255, 255]
            elif radius < .085:
                color = [c * .4 + 255 * .6 for c in color] if radius > .07 else color
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
    print(f"Radar icon: {args.output}")
