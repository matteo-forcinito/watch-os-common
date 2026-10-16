"""devcon commands without resetting the board:
python devcon_noreset.py COM10 path/to/devcon.py "open web" wait:10 shot:x.png"""
import importlib.util
import sys
import time

import serial

spec = importlib.util.spec_from_file_location('devcon', sys.argv[2])
devcon = importlib.util.module_from_spec(spec)
spec.loader.exec_module(devcon)

port = serial.Serial()
port.port, port.baudrate, port.timeout, port.write_timeout = sys.argv[1], 115200, 0.5, 3
port.dtr = port.rts = False
port.open()
time.sleep(0.3)

for arg in sys.argv[3:]:
    if arg.startswith('wait:'):
        time.sleep(float(arg[5:]))
    elif arg.startswith('shot:'):
        images = devcon.decode_images(devcon.run(port, 'shot', timeout=90))
        if 'screen' not in images:
            print('shot failed')
            continue
        rows = devcon.to_rgb(images['screen'])
        if 'top' in images:
            rows = devcon.to_rgb(images['top'], background=rows)
        with open(arg[5:], 'wb') as f:
            f.write(devcon.png_bytes(images['screen']['w'], images['screen']['h'], rows))
        print('saved', arg[5:])
    else:
        for line in devcon.run(port, arg):
            print(line)
port.close()
