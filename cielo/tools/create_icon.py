"""Render the Cielo icon (sky disc, range rings, aircraft arrows coloured by altitude) as a 192px PNG.

    python cielo/tools/create_icon.py external_apps/cielo/icon.png
"""
import argparse
import math
from pathlib import Path
import struct
import zlib

# (x, y, heading degrees clockwise from north, colour)
PLANES = [(-.30, -.28, 70, (181, 156, 255)), (.34, .12, 220, (124, 196, 255)), (-.05, .40, 330, (255, 200, 107)),
          (.12, -.45, 140, (159, 227, 193))]


def inside_triangle(px, py, a, b, c):
    def side(p, q, r):
        return (p[0] - r[0]) * (q[1] - r[1]) - (q[0] - r[0]) * (p[1] - r[1])
    d1, d2, d3 = side((px, py), a, b), side((px, py), b, c), side((px, py), c, a)
    return not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0))


def render():
    side = 192
    pixels = bytearray()
    shapes = []
    for x, y, heading, color in PLANES:
        h = math.radians(heading)
        fx, fy = math.sin(h), -math.cos(h)  # forward on screen
        rx, ry = -fy, fx
        size = .085
        nose = (x + fx * size, y + fy * size)
        left = (x - fx * size * .8 + rx * size * .7, y - fy * size * .8 + ry * size * .7)
        right = (x - fx * size * .8 - rx * size * .7, y - fy * size * .8 - ry * size * .7)
        notch = (x - fx * size * .35, y - fy * size * .35)
        shapes.append((nose, left, notch, right, color, (x, y, fx, fy)))
    for row in range(side):
        pixels.append(0)
        for col in range(side):
            px, py = (col - 95.5) / 96, (row - 95.5) / 96
            radius = math.hypot(px, py)
            color = [3, 6, 13]
            if radius < .9:
                t = radius / .9
                color = [7 + 6 * (1 - t), 16 + 10 * (1 - t), 31 + 18 * (1 - t)]
            for ring in (.9, .6, .3):
                if abs(radius - ring) < .012:
                    color = [43, 61, 92] if ring == .9 else [24, 38, 62]
            for nose, left, notch, right, tint, (x, y, fx, fy) in shapes:
                # Trail behind the aircraft.
                along = (px - x) * -fx + (py - y) * -fy
                across = abs((px - x) * -fy + (py - y) * fx)
                if .08 < along < .32 and across < .012:
                    fade = 1 - (along - .08) / .24
                    color = [color[i] + (tint[i] - color[i]) * .45 * fade for i in range(3)]
                if inside_triangle(px, py, nose, left, notch) or inside_triangle(px, py, nose, notch, right):
                    color = list(tint)
            if math.hypot(px, py) < .035:
                color = [238, 243, 250]
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
    print(f"Cielo icon: {args.output}")
