"""Verify header hit-testing and Studio navigation using actual pointer input."""
import importlib.util,json,sys,time
from pathlib import Path
import serial
root=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('d',root/'ESP32-S3-Knob-Touch-LCD-1.8/tools/devcon.py');d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)
port,board=sys.argv[1:]
p=serial.Serial();p.port=port;p.baudrate=115200;p.timeout=.2;p.write_timeout=3;p.dtr=p.rts=False;p.open()
history=[]
def run(cmd):
    lines=d.run(p,cmd,timeout=3);history.append({'command':cmd,'response':lines});time.sleep(.15);return lines
def view(expected):
    for attempt in range(3):
        if f'@VIEW studio={expected}' in run('studio'):return
    raise AssertionError(f'Expected Studio view {expected}')
def press(x,y):
    if board=='amoled':x,y=x*368//360,y*448//360
    run(f'press {x} {y} 100')
try:
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        if any(l.startswith('@STUDIO ') for l in run('studio')):break
    else:raise RuntimeError('Solco unavailable')
    run('studio-view -1');view(-1)
    press(180,22);view(0)
    press(180,150 if board=='knob' else 130);view(1)
    press(86 if board=='knob' else 74,46 if board=='knob' else 25);view(-1)
    press(180,22);view(0)
    press(180,196 if board=='knob' else 178);view(2)
    press(86 if board=='knob' else 74,46 if board=='knob' else 25);view(-1)
    press(180,22);view(0)
    print(f'PASS {port}: header -> Studio -> drumpad; header -> Studio -> instruments; return to menu')
finally:
    p.close();(root/'progress-files'/f'solco-{board}-header-touch-test.json').write_text(json.dumps(history,indent=2),encoding='utf-8')
