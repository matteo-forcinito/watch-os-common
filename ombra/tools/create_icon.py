"""Render the Ombra icon (the depth ruler: one ember tick near you, others fading away) as a 192px PNG.

    python ombra/tools/create_icon.py external_apps/ombra/icon.png
"""
import argparse
import math
from pathlib import Path
import struct
import zlib

INK = (243, 238, 230)
EMBER = (255, 106, 61)
BG = (7, 7, 12)

# (x, height, colour, strength): near on the left, fading to the right.
TICKS = [(-.52, .62, EMBER, 1.0), (-.30, .44, (122, 167, 255), .85), (-.12, .52, (79, 209, 197), .7),
         (.06, .30, (242, 193, 78), .55), (.22, .38, (156, 140, 255), .42), (.38, .22, INK, .32), (.52, .16, INK, .22)]


def render():
    side = 192
    pixels = bytearray()
    for y in range(side):
        pixels.append(0)
        for x in range(side):
            px, py = (x - 95.5) / 96, (y - 95.5) / 96
            radius = math.hypot(px, py)
            # Rounded square tile with a faint warm glow at the near end.
            glow = math.exp(-(((px + .52) / .35) ** 2 + ((py - .15) / .5) ** 2))
            color = [BG[i] + glow * [40, 16, 8][i] for i in range(3)]
            base_y = .30
            if abs(py - base_y) < .012 and -.66 < px < .66:
                color = [58, 56, 72]
            for tx, height, tick, strength in TICKS:
                width = .05 if tick == EMBER else .032
                if abs(px - tx) < width / 2 and base_y - height < py < base_y:
                    # Rounded top.
                    if py < base_y - height + width / 2 and math.hypot(px - tx, py - (base_y - height + width / 2)) > width / 2:
                        continue
                    color = [BG[i] + (tick[i] - BG[i]) * strength for i in range(3)]
            # Corner mask: the launcher draws icons on black, keep the corners dark.
            if max(abs(px), abs(py)) > .97 or (radius > 1.25):
                color = list(BG)
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
    print(f"Ombra icon: {args.output}")
