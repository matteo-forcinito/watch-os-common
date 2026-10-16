"""Reset a board through RTS and print its boot log for a few seconds."""
import sys
import time

import serial

port = sys.argv[1]
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 6
pattern = sys.argv[3] if len(sys.argv) > 3 else ''
s = serial.Serial(port, 115200, timeout=0.2)
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False
end = time.time() + seconds
buf = b''
while time.time() < end:
    buf += s.read(4096)
s.close()
for line in buf.decode('utf-8', 'replace').splitlines():
    if not pattern or any(p in line for p in pattern.split('|')):
        print(line)
