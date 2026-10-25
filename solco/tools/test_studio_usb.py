"""Local Studio acceptance test. Preserve user master volume and existing patterns."""
import importlib.util,json,re,sys,time
from pathlib import Path
import serial
root=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('devcon',root/'ESP32-S3-Knob-Touch-LCD-1.8/tools/devcon.py')
d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)
name,board,output=sys.argv[1:];report=Path(output);history=[]
p=serial.Serial();p.port=name;p.baudrate=115200;p.timeout=.2;p.write_timeout=3;p.dtr=p.rts=False;p.open();time.sleep(.5)
def run(command):
 p.write(b'\n');p.flush();lines=d.run(p,command,timeout=3);time.sleep(.03)
 history.append({'command':command,'response':lines,'time':time.monotonic()})
 report.write_text(json.dumps(history,indent=2),encoding='utf-8')
 if any(x.startswith('@ERR') for x in lines):raise RuntimeError(lines)
 return lines

def state():
 for attempt in range(4):
  groups={}
  for line in run('studio'):
   if line.startswith(('@STUDIO','@MOTION')):groups[line.split()[0]]=dict(re.findall(r'(\w+)=([^ ]+)',line))
  if {'@STUDIO','@MOTION'}.issubset(groups):return groups
 raise RuntimeError('Incomplete Studio status')
def event(kind,value=0,velocity=0):run(f'studio-event {kind} {value} {velocity}')
try:
 deadline=time.monotonic()+30;p.write_timeout=.5
 while time.monotonic()<deadline:
  lines=d.run(p,'studio',timeout=1)
  if any(line.startswith('@STUDIO ') for line in lines):break
  time.sleep(.2)
 else:raise RuntimeError('Studio did not start within 30 seconds')
 p.write_timeout=3
 run('loglevel 2');before=state()['@STUDIO']
 assert before['play']==before['rec']=='0',before
 assert set(before['pattern'].split('/'))=={'0'},'Preserve existing user pattern; test stopped'
 before_hits=int(before['hits'])
 event(7,0);assert state()['@STUDIO']['kit']=='0'
 event(1,0,110);event(1,5,110)
 event(7,1);assert state()['@STUDIO']['kit']=='1'
 assert int(state()['@STUDIO']['hits'])==before_hits+2
 before_hits+=2
 for bank in range(32):event(1,bank,110)
 time.sleep(.3);assert int(state()['@STUDIO']['hits'])==before_hits+32
 run('studio-burst');time.sleep(1.2);assert int(state()['@STUDIO']['hits'])==before_hits+64
 event(3)
 for pad in (0,1,3,6):event(1,pad,110);time.sleep(.12)
 event(3);recorded=state()['@STUDIO'];assert recorded['play']=='1' and recorded['rec']=='0'
 assert any(int(v) for v in recorded['pattern'].split('/'))
 time.sleep(2.2);assert int(state()['@STUDIO']['hits'])>int(recorded['hits'])
 assert any('match=1' in line for line in run('studio-saved'))
 event(2);event(4)
 run('studio-motion 1');time.sleep(1.2)
 motion=state()['@MOTION']
 if board=='amoled':assert int(motion['reads'])>=50 and motion['errors']=='0',motion
 else:assert motion['mode']=='0' and motion['reads']=='0',motion
 run('studio-motion 0');run('studio-view 1')
 hits=int(state()['@STUDIO']['hits'])
 px,py=(114,143) if board=='amoled' else (112,145)
 run(f'press {px} {py} 100');time.sleep(.2)
 assert int(state()['@STUDIO']['hits'])==hits+1
 for attempt in range(4):
  audio=dict(re.findall(r'(\w+)=([-\d/]+)',' '.join(run('solco'))))
  if {'empty','errors','clips','late','mix_us','cycle_us'}.issubset(audio):break
 else:raise RuntimeError('Incomplete audio diagnostic')
 assert audio['empty']==audio['errors']==audio['clips']==audio['late']=='0',audio
 run('loglevel 3');run('health');run('loglevel 2')
 print(f'PASS {name}: 32 banks, record/loop/clear, IMU capability, real touch pad')
finally:
 try:run('studio-motion 0')
 except Exception:pass
 p.close()
