"""Actual Key Lock UI, scratch bypass and two-deck SD render stress."""
import importlib.util,json,re,sys,time
from pathlib import Path
import serial
port,board=sys.argv[1:];history=[]
spec=importlib.util.spec_from_file_location('d',Path('ESP32-S3-Knob-Touch-LCD-1.8/tools/devcon.py'));d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)
p=serial.Serial();p.port=port;p.baudrate=115200;p.timeout=.2;p.write_timeout=3;p.dtr=p.rts=False;p.open()
def run(cmd):
 lines=d.run(p,cmd,timeout=5);history.append({'command':cmd,'response':lines});return lines
def state():
 for unused in range(5):
  v=dict(re.findall(r'(\w+)=([-\d/]+)',' '.join(run('solco'))))
  if {'enabled','value','playing','volume','empty','late','active','ready','busy'}.issubset(v):return v
 raise AssertionError(v)
def press(x,y):
 if board=='amoled':x,y=x*368//360,y*448//360
 run(f'press {x} {y} 200');time.sleep(.4)
def menu(deck):
 run(f'deck-open {deck}');time.sleep(.4);press(180,180)
try:
 run('loglevel 2');initial=state();assert initial['playing']=='0/0'
 assert initial['enabled']=='1/1',initial
 menu(0);press(250,92);press(180,323);assert state()['enabled']=='0/1'
 press(180,323);assert state()['enabled']=='1/1'
 run('solco-load-stress');time.sleep(2)
 deadline=time.monotonic()+90
 while time.monotonic()<deadline:
  prepared=state()
  if prepared['ready']=='1/1' and prepared['busy']=='0/0':break
  time.sleep(.5)
 else:raise AssertionError('MP3/WAV preparation timeout')
 run('deck-loop 0 0');run('deck-loop 1 0')
 run('deck-rate 0 131072');run('deck-rate 1 98304')
 press(112,53);press(84,153)
 menu(1);press(84,153)
 run('deck-loop 0 1');time.sleep(2.5)
 run('deck-loop 1 1');time.sleep(.5)
 run('deck-rate 0 -131072');run('deck-rate 1 32768')
 for unused in range(12):
  time.sleep(1);v=state();assert v['playing']=='1/1' and v['active']=='1/1',v
  assert v['enabled']=='1/1'
  assert v['empty']==v['errors']==v['clips']==v['late']=='0',v
 # Turn one deck off/on while both play. No radio or volume changes.
 run('deck-keylock 0 0');time.sleep(1);assert state()['enabled']=='0/1'
 run('deck-keylock 0 1');time.sleep(1)
 menu(1);press(84,153);menu(0);press(84,153)
 run('deck-rate 0 65536');run('deck-rate 1 65536');run('deck-loop 0 2');run('deck-loop 1 2')
 press(180,153);menu(1);press(180,153)
 final=state();assert final['playing']=='0/0'
 assert final['volume']==initial['volume']
 assert final['empty']==final['errors']==final['clips']==final['late']=='0',final
 run('studio-view 1')
 print('PASS',port,'Key Lock UI toggle, two SD decks -2x/+0.5x, live mode changes, clean counters',final)
finally:
 p.close();Path(f'progress-files/solco-{board}-dev9-keylock.json').write_text(json.dumps(history,indent=2),encoding='utf-8')
