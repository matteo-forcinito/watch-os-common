"""Capture a complete device frame, retrying truncated USB transfers."""
import importlib.util
from pathlib import Path
import sys
import serial

port_name, helper_path, output = sys.argv[1:]
spec = importlib.util.spec_from_file_location("devcon", helper_path)
devcon = importlib.util.module_from_spec(spec)
spec.loader.exec_module(devcon)
port = serial.Serial()
port.port, port.baudrate, port.timeout, port.write_timeout = port_name, 115200, 0.5, 3
port.dtr = port.rts = False
port.open()
try:
    devcon.run(port, "loglevel 2")
    for attempt in range(1, 5):
        frames = devcon.decode_images(devcon.run(port, "shot", timeout=90))
        screen = frames.get("screen")
        if not screen or any(len(screen["rows"].get(row, b"")) != screen["w"] * screen["bpp"]
                             for row in range(screen["h"])):
            print(f"Incomplete frame, retry {attempt}/4", flush=True)
            continue
        rows = devcon.to_rgb(screen)
        if "top" in frames:
            rows = devcon.to_rgb(frames["top"], background=rows)
        Path(output).write_bytes(devcon.png_bytes(screen["w"], screen["h"], rows))
        print(f"Complete frame saved: {output}", flush=True)
        break
    else:
        raise SystemExit("No complete frame received")
finally:
    devcon.run(port, "loglevel 3")
    port.close()
