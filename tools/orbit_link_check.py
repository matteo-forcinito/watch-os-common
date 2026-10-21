"""Hardware check of the Orbit Link API on a real device.

    python tools/orbit_link_check.py http://192.168.1.50 <token|code>

A 6 digit argument is a pairing code (traded for a token); a 32 character one
is a token. Runs every Link route once, plays a short chime, captures one
second of microphone and prints a summary. Uses only the standard library.
"""

import json
import math
import struct
import sys
import time
import urllib.error
import urllib.request


def request(base, token, path, body=None, raw=None, timeout=12):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(base.rstrip("/") + path, data=data, method="POST" if data is not None else "GET")
    if token:
        req.add_header("X-Key", token)
    if raw is not None:
        req.add_header("Content-Type", "application/octet-stream")
    elif body is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def tone(hz, ms, rate=16000, volume=0.4):
    n = rate * ms // 1000
    return b"".join(struct.pack("<h", int(math.sin(2 * math.pi * hz * i / rate) * volume * 32767)) for i in range(n))


def main():
    base, secret = sys.argv[1], sys.argv[2]
    token = secret
    if len(secret) == 6:
        code, body = request(base, "", "/api/pair", {"code": secret})
        assert code == 200, (code, body)
        token = json.loads(body)["token"]
        print("token", token)
    results = []

    def check(name, path, body=None, raw=None, expect=200):
        t = time.time()
        code, data = request(base, token, path, body, raw)
        ms = (time.time() - t) * 1000
        ok = code == expect
        results.append((ok, name, code, ms))
        print(f"{'OK ' if ok else 'ERR'} {name:<28} {code} {ms:6.0f} ms  {data[:120]!r}")
        return data

    info = json.loads(check("link", "/api/link"))
    print("features:", info.get("features"))
    check("events baseline", "/api/link/events")
    for scene in [
        {"scene": "pulse", "color": 0x3CF0FF},
        {"scene": "beam", "phase": "out", "angle": 0, "color": 0x9070FF},
        {"scene": "beam", "phase": "in", "angle": 180, "color": 0x9070FF},
        {"scene": "aura", "color": 0xFF3DCB, "hold_ms": 2000},
        {"scene": "text", "text": "Ciao dal 3026", "color": 0x3CF0FF, "hold_ms": 2500},
        {"scene": "orbi", "action": "enter", "angle": 180, "mood": "happy", "name": "Orbi", "color": 0x3CF0FF, "level": 3},
        {"scene": "orbi", "action": "eat", "mood": "calm", "name": "Orbi", "color": 0xB4FF5A, "level": 3},
        {"scene": "dial", "text": "Volume", "value": 40, "color": 0xFFB547, "hold_ms": 0},
        {"scene": "clear"},
    ]:
        check(f"scene {scene['scene']}", "/api/link/scene", scene)
        time.sleep(1.2)
    check("unknown scene refused", "/api/link/scene", {"scene": "nope"}, expect=400)
    check("timer start", "/api/timer", {"action": "start", "seconds": 90})
    check("timer state", "/api/timer")
    check("timer stop", "/api/timer", {"action": "stop"})
    if "speaker" in info.get("features", []):
        chime = tone(659, 140) + tone(880, 140) + tone(1318, 320)
        check("speaker chime", "/api/link/speaker", raw=chime)
        check("speaker odd length refused", "/api/link/speaker", raw=b"\x00", expect=400)
        time.sleep(2.5)
    if "mic" in info.get("features", []):
        check("mic on", "/api/link/mic", {"on": True})
        time.sleep(1.2)
        data = check("mic read", "/api/link/mic")
        if data[:4] == b"OMIC":
            end = struct.unpack("<I", data[4:8])[0]
            samples = struct.unpack(f"<{(len(data) - 8) // 2}h", data[8:])
            peak = max((abs(s) for s in samples), default=0)
            print(f"    mic: end={end} samples={len(samples)} peak={peak}")
        check("mic off", "/api/link/mic", {"on": False})
    if "voice" in info.get("features", []):
        check("voice list", "/api/voice")
    failed = [r for r in results if not r[0]]
    print(f"\n{len(results) - len(failed)}/{len(results)} OK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
