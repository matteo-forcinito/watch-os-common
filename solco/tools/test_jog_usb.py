"""Encoder routes, return to PLAY/paused, volume drawer and EOF on the Knob."""
import importlib.util,json,re,sys,time
from pathlib import Path
import serial
name,helper,report=sys.argv[1:4]
allow_volume='--allow-user-volume' in sys.argv
spec=importlib.util.spec_from_file_location('devcon',helper);d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)
p=serial.Serial();p.port=name;p.baudrate=115200;p.timeout=.3;p.write_timeout=3;p.dtr=p.rts=False;p.open();time.sleep(.5)
h=[]
def run(c):
 p.write(b'\n');p.flush();r=d.run(p,c,timeout=2);h.append({'command':c,'response':r,'time':round(time.monotonic(),3)});Path(report).write_text(json.dumps(h,indent=2),encoding='utf-8');return r
def state():
 for attempt in range(4):
  r=run('solco')
  if any(x.startswith('@ERROR') for x in r):raise RuntimeError(r)
  v=dict(re.findall(r'(\w+)=([-\d/]+)',' '.join(r)))
  if 'volume' in v and not allow_volume and v['volume']!='0':raise RuntimeError('User volume changed')
  if {'volume','playing','ready','held','pos','wheel','late'}.issubset(v):
   assert v['empty']==v['errors']==v['clips']==v['late']=='0',v
   return v
 raise RuntimeError('Incomplete USB status')
def press(x,y):
 state();run(f'press {x} {y} 220');time.sleep(.2)
def jog(n):
 run(f'knob {n}');time.sleep(.35);return state()
try:
 run('loglevel 2');state()
 # Close either the detail view or library, then load the original MP3.
 run('press 106 55 100');time.sleep(.15)
 run('solco-load-mp3');time.sleep(.8)
 assert state()['ready']=='1/1'
 press(143,115);time.sleep(1);press(143,115)
 press(290,75)
 if state()['wheel']!='1': press(180,214)
 before=state();vol=before['volume'];pos=int(before['pos'].split('/')[0])
 assert before['wheel']=='1' and pos>20000,before
 reverse=jog(-4)
 assert reverse['volume']==vol and reverse['playing']=='0/0' and reverse['held']=='0/0',reverse
 assert int(reverse['pos'].split('/')[0])<pos-4000,(before,reverse)
 forward=jog(4)
 assert int(forward['pos'].split('/')[0])>int(reverse['pos'].split('/')[0])+4000,(reverse,forward)
 # Center label explicitly selects volume without leaving the platter.
 press(180,214);assert state()['wheel']=='0'
 original=state();step=2 if int(original['volume'])<98 else -2
 changed=jog(step);assert int(changed['volume'])==int(original['volume'])+step,changed
 assert abs(int(changed['pos'].split('/')[0])-int(original['pos'].split('/')[0]))<256
 restored=jog(-step);assert restored['volume']==original['volume']
 press(180,214);assert state()['wheel']=='1'
 # The open volume drawer takes encoder priority, even in VINYL mode.
 press(258,55);original=state();changed=jog(step)
 assert int(changed['volume'])==int(original['volume'])+step,changed
 restored=jog(-step);assert restored['volume']==original['volume']
 press(258,55)
 press(106,307);time.sleep(.3);jog(-3)
 resumed=state();assert resumed['playing']=='1/0' and resumed['held']=='0/0',resumed
 press(180,307);time.sleep(.3)
 # A prepared MP3 must reach EOF without looking past the final WAV frame.
 press(106,307)
 end=time.monotonic()+12
 while time.monotonic()<end:
  final=state()
  if final['playing']=='0/0':break
  time.sleep(.5)
 else:raise RuntimeError('MP3 EOF timeout')
 assert final['pos'].startswith('352800/'),final
 eof_reverse=jog(-4)
 assert int(eof_reverse['pos'].split('/')[0])<348000,eof_reverse
 assert eof_reverse['playing']=='0/0' and eof_reverse['held']=='0/0',eof_reverse
 print(json.dumps({'result':'PASS','reverse_before':pos,'reverse_after':reverse['pos'],'forward_after':forward['pos'],'eof_reverse':eof_reverse,'final':state()},indent=2),flush=True)
finally:
 run('loglevel 3');p.close()
