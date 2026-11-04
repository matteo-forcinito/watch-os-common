"""Build Astri's sky assets from the d3-celestial data (BSD-3, Olaf Frohn).

Usage: py -I build_data.py <d3-celestial data dir> <astri/assets dir>

Inputs (from https://github.com/ofrohn/d3-celestial/tree/master/data):
  stars.6.json constellations.lines.json constellations.json mw.json
  messier.json starnames.json LICENSE

Outputs (little endian; RA as u16 = deg * 65536 / 360, Dec as i16 = deg * 32767 / 90):
  stars.bin   "AST1" u32 n, n x {u16 ra, i16 dec, i8 mag*10, u8 bv index}, brightest first
  lines.bin   "ASL1" u32 n, n x {u16 ra1, i16 dec1, u16 ra2, i16 dec2}
  labels.bin  "ASN1" u32 n, n x {u16 ra, i16 dec, u8 kind, i8 mag*10, u8 len, len bytes UTF-8}
              kind 0 constellation, 1 star, 2 deep sky
  mw.bin      "ASM1" u16 w, u16 h, w*h u8 brightness, RA 0..360 left to right, Dec +90 top
"""
import json
import math
import os
import shutil
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)


def load(name):
    with open(os.path.join(src, name), encoding="utf-8") as f:
        return json.load(f)


def ra_q(lon):
    ra = lon % 360.0                       # d3-celestial stores RA as -180..180
    return int(round(ra * 65536 / 360)) & 0xFFFF


def dec_q(lat):
    return max(-32767, min(32767, int(round(lat * 32767 / 90))))


def mag_q(mag):
    return max(-128, min(127, int(round(mag * 10))))


# ---- stars
stars = []
for f in load("stars.6.json")["features"]:
    mag = f["properties"]["mag"]
    try:
        bv = float(f["properties"].get("bv") or 0.6)
    except ValueError:
        bv = 0.6
    lon, lat = f["geometry"]["coordinates"]
    stars.append((mag, ra_q(lon), dec_q(lat), max(0, min(255, int((bv + 0.4) / 2.4 * 255))), f["id"], lon, lat))
stars.sort(key=lambda s: s[0])
with open(os.path.join(out, "stars.bin"), "wb") as f:
    f.write(b"AST1" + struct.pack("<I", len(stars)))
    for mag, ra, dec, bv, *_ in stars:
        f.write(struct.pack("<HhbB", ra, dec, mag_q(mag), bv))

# ---- constellation lines
segments = []
for f in load("constellations.lines.json")["features"]:
    for line in f["geometry"]["coordinates"]:
        for a, b in zip(line, line[1:]):
            segments.append((ra_q(a[0]), dec_q(a[1]), ra_q(b[0]), dec_q(b[1])))
with open(os.path.join(out, "lines.bin"), "wb") as f:
    f.write(b"ASL1" + struct.pack("<I", len(segments)))
    for s in segments:
        f.write(struct.pack("<HhHh", *s))

# ---- labels: Italian constellation names, proper names of bright stars, Messier objects
labels = []
for f in load("constellations.json")["features"]:
    p = f["properties"]
    ra, dec = p["display"][0], p["display"][1]
    labels.append((ra_q(ra), dec_q(dec), 0, 0, (p.get("it") or p["name"]).upper()))
names = load("starnames.json")
for mag, ra, dec, bv, hip, lon, lat in stars:
    entry = names.get(str(hip))
    if mag <= 2.6 and entry and entry.get("name"):
        labels.append((ra, dec, 1, mag_q(mag), entry["name"]))
for f in load("messier.json")["features"]:
    p = f["properties"]
    lon, lat = f["geometry"]["coordinates"]
    if p.get("mag", 99) <= 7.0:
        labels.append((ra_q(lon), dec_q(lat), 2, mag_q(p["mag"]), p["name"]))
with open(os.path.join(out, "labels.bin"), "wb") as f:
    f.write(b"ASN1" + struct.pack("<I", len(labels)))
    for ra, dec, kind, mag, text in labels:
        # No-break spaces (and other exotic blanks) are not in the watch fonts.
        text = " ".join(text.replace(" ", " ").replace(" ", " ").split())
        raw = text.encode("utf-8")[:40]
        f.write(struct.pack("<HhBbB", ra, dec, kind, mag, len(raw)) + raw)

# ---- Milky Way: the five d3-celestial brightness outlines, stacked and blurred
W, H = 1024, 512
from PIL import ImageChops


def ring_mask(ring):
    """Even-odd coverage of one ring. A ring that circles the whole sky (the
    edges of the band) is closed through the north pole: XOR-ing both edges
    then leaves exactly the band between them."""
    xs, previous = [], None
    for lon, lat in ring:
        ra = lon % 360.0
        if previous is not None:
            while ra - previous > 180:
                ra -= 360
            while ra - previous < -180:
                ra += 360
        xs.append(ra)
        previous = ra
    points = [(x / 360.0 * W, (90.0 - lat) / 180.0 * H) for x, (_, lat) in zip(xs, ring)]
    if abs(xs[-1] - xs[0]) > 180:   # wraps around: close via the pole (y = 0)
        points += [(points[-1][0], -1), (points[0][0], -1)]
    mask = Image.new("1", (W, H), 0)
    draw = ImageDraw.Draw(mask)
    for shift in (-2 * W, -W, 0, W, 2 * W):
        draw.polygon([(x + shift, y) for x, y in points], fill=1)
    return mask


image = Image.new("L", (W, H), 0)
for f in load("mw.json")["features"]:
    level = Image.new("1", (W, H), 0)
    for polygon in f["geometry"]["coordinates"]:
        for ring in polygon:
            level = ImageChops.logical_xor(level, ring_mask(ring))
    layer = level.convert("L").point(lambda v: 46 if v else 0)
    image = ImageChops.add(image, layer)
image = image.filter(ImageFilter.GaussianBlur(2.5))
with open(os.path.join(out, "mw.bin"), "wb") as f:
    f.write(b"ASM1" + struct.pack("<HH", W, H) + image.tobytes())
# image.save(os.path.join(out, "mw-preview.png"))   # uncomment to check the texture

shutil.copy(os.path.join(src, "LICENSE"), os.path.join(out, "LICENSE-d3-celestial.txt"))
print("stars %d, segments %d, labels %d, milky way %dx%d" % (len(stars), len(segments), len(labels), W, H))
