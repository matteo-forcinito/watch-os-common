"""Capture Studio pages through USB without toggling native USB reset signals."""
import importlib.util,sys,time
from pathlib import Path
import serial
root=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('d',root/'ESP32-S3-Knob-Touch-LCD-1.8/tools/devcon.py');d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)
name,board,title=sys.argv[1:4];commands=sys.argv[4:]
p=serial.Serial();p.port=name;p.baudrate=115200;p.timeout=.3;p.write_timeout=3;p.dtr=p.rts=False;p.open();time.sleep(.5)
try:
 quiet=time.monotonic();deadline=quiet+20
 while time.monotonic()<deadline and time.monotonic()-quiet<.4:
  data=p.read(p.in_waiting or 1)
  if data:quiet=time.monotonic()
 for unused in range(1):
  for command in commands:
   d.run(p,command,timeout=3);time.sleep(.4)
  time.sleep(1);images={}
  for attempt in range(6):
   p.reset_input_buffer();p.write(b'shot\n');raw=bytearray();deadline=time.monotonic()+(100 if board=="watcher" else 40)
   while time.monotonic()<deadline:
    raw.extend(p.read(p.in_waiting or 1))
    if b'@END' in raw:break
   # Reassemble dropped USB chunks only when every overlapping valid row is identical.
   current=d.decode_images(raw.decode('utf-8','replace').splitlines())
   for layer,image in current.items():
    saved=images.setdefault(layer,{'w':image['w'],'h':image['h'],'bpp':image['bpp'],'rows':{}})
    assert (saved['w'],saved['h'],saved['bpp'])==(image['w'],image['h'],image['bpp'])
    for row,data in image['rows'].items():
     if len(data)!=image['w']*image['bpp']:continue
     if row in saved['rows'] and data!=saved['rows'][row]:
      saved['rows'].clear()
     saved['rows'][row]=data
   if 'screen' in images and all(len(im['rows'])==im['h'] for im in images.values()):break
  assert 'screen' in images and all(len(im['rows'])==im['h'] for im in images.values())
  for image in images.values():image['data']=b''.join(image['rows'][row] for row in range(image['h']))
  image=images['screen'];rows=d.to_rgb(image)
  if 'top' in images:rows=d.to_rgb(images['top'],background=rows)
  path=root/'progress-files'/f'solco-{board}-dev8-{title}.png'
  path.write_bytes(d.png_bytes(image['w'],image['h'],rows));print(path)

finally:p.close()
