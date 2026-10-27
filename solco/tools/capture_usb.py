"""Capture complete frames; merge only identical overlapping rows on retry."""
import importlib.util
from pathlib import Path
import sys,time
import serial

port_name,helper_path,output=sys.argv[1:]
spec=importlib.util.spec_from_file_location('devcon',helper_path)
devcon=importlib.util.module_from_spec(spec);spec.loader.exec_module(devcon)
port=serial.Serial();port.port=port_name;port.baudrate=115200;port.timeout=.2;port.write_timeout=3
port.dtr=port.rts=False;port.open()
try:
 devcon.run(port,'loglevel 2')
 images={}
 for attempt in range(6):
  port.reset_input_buffer();port.write(b'shot\n');raw=bytearray();deadline=time.monotonic()+90
  while time.monotonic()<deadline:
   raw.extend(port.read(port.in_waiting or 1))
   if b'@END' in raw:break
  current=devcon.decode_images(raw.decode('utf-8','replace').splitlines())
  for layer,image in current.items():
   saved=images.setdefault(layer,{'w':image['w'],'h':image['h'],'bpp':image['bpp'],'rows':{}})
   assert (saved['w'],saved['h'],saved['bpp'])==(image['w'],image['h'],image['bpp'])
   for row,data in image['rows'].items():
    if len(data)!=image['w']*image['bpp']:continue
    if row in saved['rows'] and data!=saved['rows'][row]:saved['rows'].clear()
    saved['rows'][row]=data
  if 'screen' not in images or not all(len(im['rows'])==im['h'] for im in images.values()):
   print('Incomplete capture, retry',attempt+1,flush=True);continue
  for image in images.values():image['data']=b''.join(image['rows'][row] for row in range(image['h']))
  screen=images['screen'];rows=devcon.to_rgb(screen)
  if 'top' in images:rows=devcon.to_rgb(images['top'],background=rows)
  Path(output).write_bytes(devcon.png_bytes(screen['w'],screen['h'],rows))
  print('Complete frame saved:',output,flush=True);break
 else:raise SystemExit('No consistent complete frame received')
finally:
 try:devcon.run(port,'loglevel 3')
 finally:port.close()
