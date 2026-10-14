"""Render the Deck icon (a 3x3 block of keycaps, one glowing) as a 192px PNG.

    python deck/tools/create_icon.py external_apps/deck/icon.png
"""
import argparse
from pathlib import Path
import struct
import zlib

ACCENTS = {(0, 0): (255, 107, 107), (1, 1): (79, 179, 255), (2, 2): (91, 214, 160), (2, 0): (247, 201, 72)}


def rounded(px, py, x0, y0, w, h, r):
    """Inside a rounded rectangle."""
    cx = min(max(px, x0 + r), x0 + w - r)
    cy = min(max(py, y0 + r), y0 + h - r)
    return (px - cx) ** 2 + (py - cy) ** 2 <= r * r


def render():
    side = 192
    pixels = bytearray()
    cap, gap, start = 46, 10, 23
    for y in range(side):
        pixels.append(0)
        for x in range(side):
            color = [9, 11, 16]
            for row in range(3):
                for col in range(3):
                    x0, y0 = start + col * (cap + gap), start + row * (cap + gap)
                    accent = ACCENTS.get((row, col))
                    if rounded(x, y, x0, y0 + 5, cap, cap, 11):
                        color = [5, 6, 10]  # key body below the cap
                    if rounded(x, y, x0, y0, cap, cap, 11):
                        shade = 1 - (y - y0) / cap * .25
                        color = [27 * shade, 31 * shade, 42 * shade]
                        if accent:
                            # Glyph: a centred dot in the key colour, and a glowing edge on one key.
                            if (x - x0 - cap / 2) ** 2 + (y - y0 - cap / 2) ** 2 < 49:
                                color = list(accent)
                            if (row, col) == (1, 1) and not rounded(x, y, x0 + 2, y0 + 2, cap - 4, cap - 4, 9):
                                color = list(accent)
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
    print(f"Deck icon: {args.output}")
