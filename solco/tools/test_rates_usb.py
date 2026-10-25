"""Actual eight-rate UI and SD forward/reverse acceptance; master volume unchanged."""
import importlib.util,json,re,sys,time
from pathlib import Path
import serial
port,board=sys.argv[1:];history=[]
sp=importlib.util.spec_from_file_location('d',Path('ESP32-S3-Knob-Touch-LCD-1.8/tools/devcon.py'));d=importlib.util.module_from_spec(sp);sp.loader.exec_module(d)
p=serial.Serial();p.port=port;p.baudrate=115200;p.timeout=.2;p.write_timeout=3;p.dtr=p.rts=False;p.open()
def run(cmd):
    lines=d.run(p,cmd,timeout=3);history.append({'command':cmd,'response':lines});return lines
def state():
    for retry in range(4):
        v=dict(re.findall(r'(\w+)=([-\d/]+)',' '.join(run('solco'))))
        if {'value','speed','pos','playing','empty','volume','late'}.issubset(v):return v
    raise AssertionError(v)
def press(x,y):
    if board=='amoled':x,y=x*368//360,y*448//360
    elif board=='watcher':x,y=x*412//360,y*412//360
    run(f'press {x} {y} 200');time.sleep(.3)
try:
    initial=state();assert initial['playing']=='0/0'
    run('deck-open 0');time.sleep(.5);press(180,180);press(250,92)
    for x,y,rate in [(108,133,-131072),(252,133,131072),(108,181,-98304),(252,181,98304),(108,229,-65536),(252,229,65536),(108,277,-32768),(252,277,32768)]:
        press(x,y);v=state();assert int(v['value'].split('/')[0])==rate,v
        assert v['playing']=='0/0'
    # A >3-second SD loop prevents serial-console delays from reaching EOF/BOF.
    press(252,133);press(238,53);press(180,180);press(84,205);press(84,153);time.sleep(1.7);press(180,205)
    v=state();assert v['active']=='1/0',v
    upper=int(v['out'].split('/')[0]);assert upper>132300,upper
    press(250,92);press(108,133)
    for i in range(4):
        time.sleep(1);v=state()
        assert v['playing']=='1/0' and v['speed'].split('/')[0]=='-131072',v
        assert 0<=int(v['pos'].split('/')[0])<=upper,v
    # Resize while reversing: new edge pages must be prefetched before publication.
    press(112,53);press(94,257);v=state();assert int(v['out'].split('/')[0])<upper,v
    time.sleep(1);press(180,257);assert abs(int(state()['out'].split('/')[0])-upper)<=1
    press(250,92);press(252,277);time.sleep(.4)
    v=state();assert v['playing']=='1/0' and v['speed'].split('/')[0]=='32768',v
    press(112,53);press(84,153);assert state()['playing']=='0/0'
    run('deck-rate 0 65536');press(180,153);time.sleep(.4)
    final=state();assert final['empty']==final['errors']==final['clips']==final['late']=='0',final
    assert final['volume']==initial['volume']
    run('studio-view 1')
    print('PASS',port,'eight UI rates, paused selection, SD +2x/-2x/+0.5x, return +1x, clean audio',final)
finally:
    p.close();Path(f'progress-files/solco-{board}-dev8-rates.json').write_text(json.dumps(history,indent=2),encoding='utf-8')
