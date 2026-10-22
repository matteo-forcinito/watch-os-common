"""Reference implementation of the Orbi genome (docs/ORBI-GENOME.md).

    python tools/orbi_genome.py <seed hex>      traits of one creature
    python tools/orbi_genome.py --vectors       JSON test vectors for C and Dart
"""
import json
import sys

MASK = (1 << 64) - 1
SYLLABLES = ("lu mi ko na ri te vo sa ni po ra zu be di ke la mo nu pi ru "
             "si ta ve yo fi go ma ne ro tu bi da ki le mu no pa re so ti").split()

CAP_ROUND, CAP_SQUARE, CAP_MIC, CAP_SPEAKER, CAP_IMU, CAP_KNOB, CAP_CAMERA = (1 << i for i in range(7))
BODIES = ["DROP", "CUBE", "CAPSULE"]
EARS = ["NONE", "ROUND_EARS", "POINTY_EARS", "NUBS"]
CRESTS = ["NONE", "SPROUT", "HORN", "ANTENNA", "NOTE"]
EYES = ["ROUND", "OVAL", "WIDE", "SLEEPY", "SPARKLE", "CYCLOPS"]
PATTERNS = ["NONE", "SPOTS", "STRIPES", "BELLY", "FRECKLES"]


def fnv1a32(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


class SplitMix:
    def __init__(self, state):
        self.state = state & MASK

    def next(self):
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
        return z ^ (z >> 31)

    def pick(self, n):
        return self.next() % n


def make_seed(caps, mac, model, index, birth):
    device = fnv1a32(bytes(mac) + model.encode()) & 0xFFFFFF
    individual = fnv1a32(bytes([index & 0xFF]) + (birth & 0xFFFFFFFF).to_bytes(4, "little"))
    return ((caps & 0xFF) << 56) | (device << 32) | individual


def traits(seed):
    caps = seed >> 56
    fam = SplitMix(seed >> 32)
    t = {"seed": "%016x" % seed, "caps": caps}
    t["hue"] = fam.pick(360)
    t["body"] = "CUBE" if caps & CAP_SQUARE else "DROP" if caps & CAP_ROUND else "CAPSULE"
    t["stretch"] = 90 + fam.pick(21)
    if caps & CAP_CAMERA:
        t["ear"] = "NONE"
    elif caps & CAP_MIC:
        t["ear"] = "ROUND_EARS" if fam.pick(2) else "POINTY_EARS"
    else:
        t["ear"] = "NUBS" if fam.pick(3) == 0 else "NONE"
    if caps & CAP_KNOB:
        t["crest"] = "ANTENNA"
    elif caps & CAP_SPEAKER:
        t["crest"] = "NOTE" if fam.pick(2) else "SPROUT"
    else:
        t["crest"] = ["NONE", "SPROUT", "HORN"][fam.pick(3)]

    ind = SplitMix(seed)
    t["hue2"] = (t["hue"] + 300 + ind.pick(121)) % 360
    t["eyes"] = "CYCLOPS" if caps & CAP_CAMERA else ["ROUND", "OVAL", "WIDE", "SLEEPY", "SPARKLE"][ind.pick(5)]
    t["eye_gap"] = 34 + ind.pick(15)
    t["eye_size"] = 14 + ind.pick(9)
    t["pattern"] = PATTERNS[ind.pick(5)]
    t["cheeks"] = ind.pick(3) != 0
    for key in ("curiosity", "energy", "sociability", "appetite", "playfulness"):
        t[key] = 20 + ind.pick(81)
    count = 2 + (1 if ind.pick(4) == 0 else 0)
    name = "".join(SYLLABLES[ind.pick(40)] for _ in range(count))
    name += ["", "", "", "n", "l", "x"][ind.pick(6)]
    t["name"] = name[0].upper() + name[1:]
    return t


def vectors():
    cases = [
        (CAP_ROUND | CAP_MIC | CAP_SPEAKER | CAP_IMU, [0xA0, 0xF2, 0x62, 0xE3, 0x07, 0x94], "amoled175c", 0, 1761100000),
        (CAP_ROUND | CAP_MIC | CAP_SPEAKER | CAP_IMU, [0xA0, 0xF2, 0x62, 0xE3, 0x07, 0x94], "amoled175c", 1, 1761200000),
        (CAP_SQUARE | CAP_MIC | CAP_SPEAKER, [0xDC, 0xB4, 0xD9, 0x04, 0x23, 0x4C], "guition4848", 0, 1761100000),
        (CAP_ROUND | CAP_KNOB | CAP_SPEAKER, [0x10, 0x20, 0x30, 0x40, 0x50, 0x60], "knob", 0, 1700000000),
        (CAP_ROUND | CAP_CAMERA | CAP_MIC | CAP_SPEAKER, [0x11, 0x22, 0x33, 0x44, 0x55, 0x66], "watcher", 0, 1750000000),
        (CAP_IMU, [0xCC, 0xBA, 0x97, 0x11, 0x8B, 0x08], "lcd13", 0, 1761150000),
    ]
    out = []
    for caps, mac, model, index, birth in cases:
        seed = make_seed(caps, mac, model, index, birth)
        out.append({"input": {"caps": caps, "mac": "".join("%02x" % b for b in mac), "model": model, "index": index,
                              "birth": birth}, "traits": traits(seed)})
    # Raw generator check.
    g = SplitMix(1)
    out.append({"splitmix_state_1": ["%016x" % g.next() for _ in range(3)]})
    return out


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--vectors":
        print(json.dumps(vectors(), indent=1))
    elif len(sys.argv) > 1:
        print(json.dumps(traits(int(sys.argv[1], 16)), indent=1))
    else:
        print(__doc__)
