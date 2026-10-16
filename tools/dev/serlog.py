"""Read a board's log without resetting it: python serlog.py COM10 seconds [out-file]"""
import sys
import time

import serial

s = serial.Serial()
s.port, s.baudrate, s.timeout = sys.argv[1], 115200, 0.2
s.dtr = s.rts = False
s.open()
end = time.time() + float(sys.argv[2])
buf = bytearray()
while time.time() < end:
    buf += s.read(8192)
s.close()
text = buf.decode('utf-8', 'replace')
if len(sys.argv) > 3:
    open(sys.argv[3], 'w', encoding='utf-8').write(text)
print(text)
