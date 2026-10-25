"""Muted hardware acceptance test. Abort if the user changes the volume."""
import importlib.util
import json
from pathlib import Path
import re
import sys
import time
import serial

port_name, helper_path, board, report = sys.argv[1:]
spec = importlib.util.spec_from_file_location("devcon", helper_path)
devcon = importlib.util.module_from_spec(spec)
spec.loader.exec_module(devcon)
port = serial.Serial()
port.port, port.baudrate, port.timeout, port.write_timeout = port_name, 115200, 0.5, 3
port.dtr = port.rts = False
port.open()
time.sleep(0.3)
history = []

def run(command):
    lines = devcon.run(port, command, timeout=2)
    time.sleep(0.15)
    history.append({"command": command, "response": lines})
    return lines

def status():
    values = {}
    for attempt in range(3):
        lines = run("solco")
        values = dict(re.findall(r"(\w+)=([-\d/]+)", " ".join(lines)))
        if "volume" in values and values["volume"] != "0":
            raise RuntimeError("User volume changed; test stopped")
        if {"volume", "pos", "held", "ready", "playing", "speed"}.issubset(values):
            break
    else:
        raise RuntimeError("Incomplete console frames; test stopped")
    assert values["errors"] == "0" and values["clips"] == "0", values
    return values

def action(command, delay=0.2):
    status()
    run(command)
    time.sleep(delay)

held = False
try:
    run("loglevel 2")
    initial = status()
    assert initial["playing"] == "0/0", "User started playback; test stopped"
    run("solco-fixture")  # O_EXCL: existing files are preserved.
    action("solco-load-test", 1)
    assert status()["ready"] == "1/1"
    if board == "amoled":
        play_a, play_b, open_a = (130, 139), (130, 256), (320, 80)
        detail_play, cx, cy, radius = (92, 312), 184, 185, 56
        back = (54, 36)
    else:
        play_a, play_b, open_a = (143, 115), (143, 203), (290, 75)
        detail_play, cx, cy, radius = (90, 250), 180, 159, 42
        back = (90, 65)
    action(f"press {play_a[0]} {play_a[1]} 100")
    action(f"press {play_b[0]} {play_b[1]} 100", 3)
    rolling = status()
    assert rolling["playing"].endswith("/1"), rolling
    assert int(rolling["pos"].split('/')[0]) > 100000, rolling
    action(f"press {play_a[0]} {play_a[1]} 100")
    action(f"press {open_a[0]} {open_a[1]} 100")
    before = status()
    action(f"touch {cx + radius} {cy} 1", 0.2)
    held = True
    assert status()["held"] == "1/0"
    action(f"touch {cx} {cy - radius} 1", 0.5)
    backward = status()
    assert int(backward["pos"].split('/')[0]) < int(before["pos"].split('/')[0]) - 10000
    action(f"touch {cx + radius} {cy} 1", 0.5)
    forward = status()
    assert int(forward["pos"].split('/')[0]) > int(backward["pos"].split('/')[0]) + 10000
    run(f"touch {cx + radius} {cy} 0")
    held = False
    time.sleep(0.4)
    released_paused = status()
    assert released_paused["playing"] == "0/1" and released_paused["held"] == "0/0"
    action(f"press {back[0]} {back[1]} 100")
    open_b = (320, 197) if board == "amoled" else (290, 162)
    action(f"press {open_b[0]} {open_b[1]} 100")
    action(f"touch {cx + radius} {cy} 1")
    held = True
    action(f"touch {cx} {cy - radius} 1", 0.3)
    grabbed_playing = status()
    assert grabbed_playing["held"] == "0/1", grabbed_playing
    run(f"touch {cx} {cy - radius} 0")
    held = False
    time.sleep(0.4)
    released_playing = status()
    assert released_playing["playing"] == "0/1" and released_playing["held"] == "0/0"
    assert int(released_playing["speed"].split('/')[1]) == 65536
    assert released_playing["empty"] == "0", released_playing
    action(f"press {detail_play[0]} {detail_play[1]} 100")
    action(f"press {back[0]} {back[1]} 100")
    final = status()
    assert final["playing"] == "0/0", final
    print(json.dumps({"board": board, "result": "PASS", "final": final}, indent=2))
finally:
    if held:
        run(f"touch {cx} {cy} 0")
    run("loglevel 3")
    port.close()
    Path(report).write_text(json.dumps(history, indent=2), encoding="utf-8")
