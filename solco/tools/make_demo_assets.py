"""Generate original 120 BPM test loops offline, keeping synthesis out of playback."""
import math
from pathlib import Path
import struct

destination = Path(__file__).resolve().parents[1] / "assets"
destination.mkdir(exist_ok=True)
rate = 44100
frames = rate // 2
for name in ("pulse", "chord"):
    samples = []
    for index in range(frames):
        t = index / rate
        if name == "pulse":
            value = 11000 * math.exp(-t * 22) * math.sin(2 * math.pi * (55 * t + 2 * (1 - math.exp(-t * 35))))
        else:
            envelope = (1 - math.exp(-t * 120)) * math.exp(-t * 9)
            value = 3000 * envelope * sum(math.sin(t * 2 * math.pi * note) for note in (220, 277.1826, 329.6276))
        # Close the loop at zero, without a discontinuity at the beat boundary.
        value *= min(1.0, (frames - 1 - index) / (rate * 0.01))
        samples.append(max(-32768, min(32767, int(value))))
    data = struct.pack("<" + "h" * frames, *samples)
    target = destination / f"{name}.pcm"
    if not target.exists() or target.read_bytes() != data:
        target.write_bytes(data)
    print(target.name, len(data))
