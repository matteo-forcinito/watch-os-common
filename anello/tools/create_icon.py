"""Render the Anello icon (rim, paddle arc, crystal rings, glowing ball) as a 192px PNG.

    python anello/tools/create_icon.py external_apps/anello/icon.png
"""
import argparse
import math
from pathlib import Path
import struct
import zlib

RINGS = [(.30, (255, 95, 162), 6), (.46, (162, 123, 255), 8), (.62, (79, 216, 240), 10)]


def render():
    side = 192
    pixels = bytearray()
    for row in range(side):
        pixels.append(0)
        for col in range(side):
            px, py = (col - 95.5) / 96, (row - 95.5) / 96
            radius = math.hypot(px, py)
            angle = math.degrees(math.atan2(py, px)) % 360  # clockwise from 3 o'clock on screen
            color = [5, 4, 10]
            if abs(radius - .86) < .012:
                color = [26, 24, 48]
            # Paddle at 6 o'clock.
            if abs(radius - .84) < .045 and abs(((angle - 90 + 180) % 360) - 180) < 30:
                color = [245, 240, 255]
            for ring, tint, count in RINGS:
                step = 360 / count
                local = (angle - ring * 40) % step
                if abs(radius - ring) < .045 and 6 < local < step - 6:
                    color = list(tint)
            # Core and ball with a soft glow.
            if radius < .09:
                color = [42, 36, 80] if radius < .075 else [109, 91, 208]
            ball = math.hypot(px - .22, py - .52)
            glow = math.exp(-(ball / .09) ** 2)
            color = [color[i] + (185 - color[i]) * glow * .5 for i in range(3)]
            if ball < .05:
                color = [255, 255, 255]
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
    print(f"Anello icon: {args.output}")
