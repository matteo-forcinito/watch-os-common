"""Render the Astri icon (night-sky disc, a constellation, a crescent Moon) as a 192 px PNG.

    py -I astri/tools/create_icon.py <out.png>
"""
import math
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

S = 192
SCALE = 4   # draw large, then shrink: smooth edges
N = S * SCALE

img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
r = N // 2 - 2 * SCALE
c = N // 2

# Sky: deep blue disc, lighter towards the horizon ring.
for i in range(r, 0, -SCALE):
    t = i / r
    d.ellipse([c - i, c - i, c + i, c + i], fill=(int(8 + 22 * t), int(12 + 34 * t), int(34 + 60 * t), 255))
d.ellipse([c - r, c - r, c + r, c + r], outline=(90, 120, 190, 255), width=2 * SCALE)

# Faint background stars.
random.seed(7)
for _ in range(70):
    a, rr = random.uniform(0, 2 * math.pi), r * math.sqrt(random.random()) * 0.92
    x, y = c + rr * math.cos(a), c + rr * math.sin(a)
    s = random.choice([1, 1, 1, 2]) * SCALE // 2 + 1
    v = random.randint(120, 210)
    d.ellipse([x - s, y - s, x + s, y + s], fill=(v, v, min(255, v + 30), 255))

# A constellation (Orion-like belt and shoulders, our own shape).
pts = [(-0.38, -0.30), (-0.10, -0.42), (0.20, -0.20), (0.02, 0.02), (-0.08, 0.10), (-0.18, 0.18), (0.30, 0.34)]
pts = [(c + x * r, c + y * r) for x, y in pts]
for a, b in [(0, 1), (1, 2), (2, 3), (3, 4), (4, 5), (3, 6)]:
    d.line([pts[a], pts[b]], fill=(110, 150, 220, 255), width=2 * SCALE)
for x, y in pts:
    s = 4 * SCALE
    d.ellipse([x - s, y - s, x + s, y + s], fill=(235, 240, 255, 255))

# Crescent Moon, lower right.
mx, my, mr = c + 0.46 * r, c + 0.40 * r, 0.22 * r
d.ellipse([mx - mr, my - mr, mx + mr, my + mr], fill=(245, 235, 200, 255))
d.ellipse([mx - mr + 0.45 * mr, my - mr - 0.15 * mr, mx + mr + 0.45 * mr, my + mr - 0.15 * mr],
          fill=(16, 26, 60, 255))

img = img.resize((S, S), Image.LANCZOS)
img.save(sys.argv[1])
print("saved", sys.argv[1])
