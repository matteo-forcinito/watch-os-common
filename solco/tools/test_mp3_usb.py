"""MP3 import/mix/scratch acceptance; audible tests require an explicit flag."""
import importlib.util
import json
from pathlib import Path
import re
import sys
import time
import serial

allow_volume = "--allow-user-volume" in sys.argv
args = [arg for arg in sys.argv[1:] if arg != "--allow-user-volume"]
name, helper, board, report = args
spec = importlib.util.spec_from_file_location("devcon", helper)
devcon = importlib.util.module_from_spec(spec)
spec.loader.exec_module(devcon)
port = serial.Serial()
port.port, port.baudrate, port.timeout, port.write_timeout = name, 115200, 0.5, 3
port.dtr = port.rts = False
port.open()
time.sleep(0.3)
history = []

def run(command):
    port.write(b"\n")
    port.flush()
    response = devcon.run(port, command, timeout=2)
    time.sleep(0.1)
    history.append({"command": command, "response": response})
    Path(report).write_text(json.dumps(history, indent=2), encoding="utf-8")
    return response

def status():
    for attempt in range(4):
        lines = run("solco")
        if any(line.startswith("@ERROR") for line in lines): raise RuntimeError(lines)
        v = dict(re.findall(r"(\w+)=([-\d/]+)", " ".join(lines)))
        if not allow_volume and "volume" in v and v["volume"] != "0":
            raise RuntimeError("User volume changed: automated gestures stopped")
        if {"volume", "playing", "ready", "busy", "pos", "held"}.issubset(v):
            assert v["empty"] == v["errors"] == v["clips"] == "0", v
            return v
    raise RuntimeError("Incomplete console responses")

def press(point):
    status()
    run(f"press {point[0]} {point[1]} 100")
    time.sleep(0.2)

held = False
try:
    run("loglevel 2")
    initial = status()
    assert initial["playing"] == "0/0"
    bx, by = (54, 36) if board == "amoled" else (106, 55)
    run(f"press {bx} {by} 220")
    time.sleep(.2)
    run("solco-mp3-fixture")
    assert any("match=1" in line for line in run("solco-mp3-check"))
    run("solco-load-mp3")
    time.sleep(0.15)
    play_a, play_b, open_a, back, cue = ((130, 139), (130, 256), (320, 80), (54, 36), (219, 139)) if board == "amoled" else ((143, 115), (143, 203), (290, 75), (90, 56), (214, 115))
    cx, cy, radius = (184, 263, 100) if board == "amoled" else (180, 214, 90)
    first = status()
    assert first["ready"].endswith("/1")
    observed_preparing = first["busy"].startswith("1/")
    press(play_b)
    started = time.monotonic()
    while time.monotonic() - started < 120:
        ready = status()
        assert ready["playing"].endswith("/1")
        observed_preparing |= ready["busy"].startswith("1/")
        if ready["busy"] == "0/0" and ready["ready"] != "1/1":
            raise RuntimeError("MP3 preparation failed; inspect console history")
        if ready["ready"] == "1/1" and ready["busy"] == "0/0": break
        time.sleep(0.2)
    else: raise RuntimeError("MP3 not ready after two minutes")
    assert ready["percent"].startswith("100/") or (not observed_preparing and ready["percent"].startswith("0/")), ready
    press(play_a)
    time.sleep(2)
    playing = status()
    assert int(playing["pos"].split('/')[0]) > 10000
    if playing["playing"].startswith("1/"): press(play_a)
    press(open_a)
    before = status()
    run(f"touch {cx + radius} {cy} 1")
    held = True
    time.sleep(0.3)
    assert status()["held"] == "1/0"
    run(f"touch {cx} {cy-radius} 1")
    time.sleep(0.6)
    reverse = status()
    assert int(reverse["pos"].split('/')[0]) < int(before["pos"].split('/')[0]) - 10000
    run(f"touch {cx+radius} {cy} 0")
    held = False
    press(back)
    press(cue)
    time.sleep(0.3)
    cued = status()
    assert cued["ready"] == "1/1" and cued["busy"] == "0/0"
    assert cued["pos"].startswith("0/"), cued
    press(play_b)
    final = status()
    assert final["playing"] == "0/0"
    print(json.dumps({"result": "PASS", "board": board, "observed_preparing": observed_preparing,
                      "final": final}, indent=2), flush=True)
finally:
    if held: run(f"touch {cx} {cy} 0")
    run("loglevel 3")
    port.close()
    Path(report).write_text(json.dumps(history, indent=2), encoding="utf-8")
