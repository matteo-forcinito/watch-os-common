# Orbi genome

Orbi (the father) lives in the Orbit companion; his children live on the
devices. A child is a **64-bit seed**: from it the device firmware (C) and
the companion (Dart) rebuild exactly the same creature, so a child can be
shown, moved or restored anywhere by sending only the seed (plus its life
state: age, needs, experience - not part of the genome).

Reference implementation and test vectors: `tools/orbi_genome.py`
(`python tools/orbi_genome.py --vectors` prints them). The C
(`components/orbi/orbi_genome.c`) and Dart
(`Orbit-Companion/lib/ecosystem/orbi_genome.dart`) implementations must
reproduce those vectors exactly.

## Seed layout

```
bit 63            56 55                     32 31                       0
    [  capabilities ][  device identity hash  ][  individual            ]
     \__________ family (shared by the device's children) _/
```

- **capabilities** (8 bits), what the device is - the species comes from it:
  bit 0 round screen, 1 square screen, 2 microphone, 3 speaker,
  4 motion sensor (IMU), 5 knob, 6 camera, 7 reserved (0).
- **device identity** (24 bits): FNV-1a 32 of the 6 MAC bytes followed by
  the model id text ("amoled175c"), lower 24 bits.
- **individual** (32 bits): FNV-1a 32 of `[child index (1 byte), birth time
  (4 bytes, little endian, Unix seconds)]`.

The seed is written as 16 lowercase hex digits ("orbi code").

## Random streams

SplitMix64 (64-bit wrapping arithmetic, logical shifts):

```
state += 0x9E3779B97F4A7C15
z = state
z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
z = (z ^ (z >> 27)) * 0x94D049BB133111EB
return z ^ (z >> 31)
```

`pick(n)` = `next() % n` (unsigned). Two streams:

- **family stream**, state = seed >> 32 (only the family half): species
  details shared by siblings.
- **individual stream**, state = the whole seed.

Draws happen in exactly this order (changing it changes every creature).

## Traits

Family stream:

1. `hue` = pick(360) - base colour (degrees)
2. `body` = shape from capabilities: square screen -> CUBE, round -> DROP,
   else -> CAPSULE; `stretch` = 90 + pick(21) (% height/width)
3. `ear` = from capabilities: camera -> NONE (one big eye instead),
   microphone -> pick(2) ? ROUND_EARS : POINTY_EARS, else pick(3) == 0 ?
   NUBS : NONE
4. `crest` = knob -> ANTENNA, speaker -> pick(2) ? NOTE : SPROUT, else
   [NONE, SPROUT, HORN][pick(3)]

Individual stream:

5. `hue2` = (hue + 300 + pick(121)) % 360 - secondary colour (±60 deg)
6. `eyes` = camera -> CYCLOPS, else [ROUND, OVAL, WIDE, SLEEPY, SPARKLE][pick(5)]
7. `eye_gap` = 34 + pick(15) (% of body width between eye centres)
8. `eye_size` = 14 + pick(9) (% of body width)
9. `pattern` = [NONE, SPOTS, STRIPES, BELLY, FRECKLES][pick(5)]
10. `cheeks` = pick(3) != 0
11. `curiosity`, `energy`, `sociability`, `appetite`, `playfulness` =
    20 + pick(81) each, in this order
12. `name`: syllables from SYLLABLES (40 entries, below): count = 2 +
    (pick(4) == 0 ? 1 : 0); each = SYLLABLES[pick(40)]; ending =
    ["", "", "", "n", "l", "x"][pick(6)]; first letter upper case.

```
SYLLABLES = lu mi ko na ri te vo sa ni po ra zu be di ke la mo nu pi ru
            si ta ve yo fi go ma ne ro tu bi da ki le mu no pa re so ti
```

## Life (not genome)

Kept beside the seed, changeable without touching how a child looks:
age (days), stage (egg, hatchling, young, adult), needs 0-100 (food,
energy, joy, company), experience. A neglected child gets sad and sleepy,
never dies: the rules live in one function so they can change later.

## Orbi the father

Orbi is seed `0d0000bb00000245` with the name forced to "Orbi": round,
cyan (hue 188), round ears, a note, round eyes, cheeks.

## Drawing (same on device and app)

Coordinates relative to the body width W; H = W x stretch / 100 (CAPSULE:
H = 1.25 W x stretch / 100). Colours in HSV (s, v in %):
body (hue, 55, 95), belly (hue, 25, 100), accent (hue2, 60, 90),
eyes #0A1020, highlight white, cheeks (350, 45, 100) at 60% opacity,
sprout green (120, 60, 80), shadow black 35%.

- body: rounded rect W x H; radius DROP/CAPSULE = min(W, H) / 2, CUBE = 0.28 W
- behind the body (drawn first):
  - ears ROUND: circles 0.30 W centred at (0.18 W, 0.02 H) and (0.82 W, 0.02 H), inner circle 0.16 W in accent
  - ears POINTY: rounded rects 0.16 W x 0.34 W centred at (0.22 W, 0) and (0.78 W, 0)
  - ears NUBS: circles 0.14 W centred at (0.25 W, 0.04 H) and (0.75 W, 0.04 H)
  - crest (top centre): SPROUT stem 0.03 x 0.16 W + leaf 0.14 x 0.08 W (green);
    HORN rounded rect 0.08 x 0.20 W (accent); ANTENNA stem 0.025 x 0.22 W +
    ball 0.10 W (accent); NOTE stem 0.03 x 0.20 W at +0.05 W + head 0.11 W (accent)
- pattern: BELLY ellipse 0.56 W x 0.42 H at (0.5 W, 0.70 H) in belly colour;
  SPOTS circles 0.10 W at (0.22, 0.30) (0.78, 0.26) (0.70, 0.72) (0.30, 0.78) x (W, H);
  STRIPES 3 bars 0.36 x 0.05 W at y 0.10, 0.18, 0.26 H; FRECKLES 3 dots
  0.035 W under each eye. Accent colour.
- eyes: centres x = 0.5 W +/- eye_gap% x W / 2, y = 0.42 H; e = eye_size% x W.
  ROUND e x e, OVAL 0.8e x 1.25e, WIDE 1.3e x 0.9e, SLEEPY e x 0.55e,
  SPARKLE 1.1e x 1.1e (+ a second highlight), CYCLOPS one eye 2e at the centre.
  Highlight 0.3e at (+0.15e, -0.18e). Gaze moves the eyes up to 0.12e.
- cheeks: ellipses 0.14 x 0.08 W, 0.13 W under each eye, 0.06 W further out
- mouth at (0.5 W, 0.62 H), 0.18 x 0.09 W, line 0.025 W: HAPPY a smile (lower
  edge of a rounded rect), SAD the upper edge, CALM/SLEEPY a short line,
  LOVE a wider smile (0.24 W); SLEEP closes the eyes into lines
- shadow: ellipse 0.7 x 0.08 W, 0.06 W below the body

Motion: float 2.5% W over 3 s, breathing +/-1.5% height, blink every 3-6 s
(120 ms), gaze wanders or follows touch/tilt, a poke squashes (-10% height,
+6% width) and springs back.
