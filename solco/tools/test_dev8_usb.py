"""Hardware acceptance: enlarged surfaces, real touch navigation, scratch reversal and SD loops."""
import importlib.util,json,re,sys,time
from pathlib import Path
import serial
root=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('d',root/'ESP32-S3-Knob-Touch-LCD-1.8/tools/devcon.py');d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)
port,board=sys.argv[1:];history=[]
p=serial.Serial();p.port=port;p.baudrate=115200;p.timeout=.2;p.write_timeout=3;p.dtr=p.rts=False;p.open()
def run(cmd):
    lines=d.run(p,cmd,timeout=3);history.append({'command':cmd,'response':lines,'time':time.monotonic()});return lines
def values(cmd):
    for attempt in range(4):
        lines=run(cmd);v=dict(re.findall(r'(\w+)=([-\d/]+)',' '.join(l for l in lines if not l.startswith('@MOTION'))))
        if cmd=='studio':
            v.update({'motion_'+k:val for l in lines if l.startswith('@MOTION') for k,val in re.findall(r'(\w+)=([-\d/]+)',l)})
        if (cmd=='studio' and {'studio_tools','hits','bpm','studio','motion_errors'}.issubset(v)) or (cmd=='solco' and {'active','pos','playing','empty','cycle_us'}.issubset(v)):return v
    raise AssertionError(lines)
def press(x,y):
    if board=='amoled':x,y=x*368//360,y*448//360
    elif board=='watcher':x,y=x*412//360,y*412//360
    run(f'press {x} {y} 200');time.sleep(.4)
def touch(x,y,on):
    if board=='amoled':x,y=x*368//360,y*448//360
    elif board=='watcher':x,y=x*412//360,y*412//360
    run(f'touch {x} {y} {on}');time.sleep(.2)
def view(n):assert values('studio')['studio']==str(n)
try:
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        if any(l.startswith('@STUDIO ') for l in run('studio')):break
    else:raise RuntimeError('Solco unavailable')
    run('loglevel 2');initial=values('solco');assert initial['playing']=='0/0'
    run('solco-fixture');run('solco-load-test');time.sleep(2)
    run('studio-view -1');time.sleep(.4);press(180,30);view(0)
    press(180,155);view(1)
    hits=int(values('studio')['hits']);press(106,100 if board!='amoled' else 80)
    assert int(values('studio')['hits'])==hits+1
    press(246 if board!='amoled' else 294,320 if board!='amoled' else 330)
    assert values('studio')['studio_tools']=='1'
    bpm=values('studio')['bpm'];up=int(bpm)<=195
    press(230 if up else 110,138);assert int(values('studio')['bpm'])==int(bpm)+(5 if up else -5)
    press(110 if up else 230,138);assert values('studio')['bpm']==bpm
    press(180,50);assert values('studio')['studio_tools']=='0'
    press(81 if board!='amoled' else 29,46 if board!='amoled' else 26);view(-1)
    press(180,30);press(180,205);view(2)
    hits=int(values('studio')['hits']);press(106,100 if board!='amoled' else 80)
    assert int(values('studio')['hits'])==hits+1
    run('studio-view -1');time.sleep(.4);press(290 if board!='amoled' else 314,74 if board!='amoled' else 62);time.sleep(2)
    press(180,180);assert values('studio')['deck_menu']=='1'
    press(238,53);assert values('studio')['deck_menu']=='0'
    # Paused scratch, including reversal while still touching, without a toggle.
    touch(300,180,1);touch(300,240,1);forward=int(values('solco')['pos'].split('/')[0])
    touch(300,180,1);reverse=int(values('solco')['pos'].split('/')[0]);touch(300,180,0)
    assert forward>0 and reverse<forward,(forward,reverse)
    assert values('solco')['playing']=='0/0'
    press(180,180);assert values('studio')['deck_menu']=='1'
    # Existing owned twelve-second WAV fixture, no modification of user tracks/pattern.
    press(180,153);time.sleep(.4)
    press(84,205);assert values('solco')['in'].split('/')[0]=='0'
    press(84,153);time.sleep(4.1)
    press(180,205);v=values('solco');assert v['active']=='1/0',v
    start,end=(int(v[k].split('/')[0]) for k in ('in','out'))
    assert end-start>132300,(start,end)
    for i in range(5):
        time.sleep(2);v=values('solco');assert start<=int(v['pos'].split('/')[0])<=end,v
    press(94,257);half=values('solco');assert int(half['out'].split('/')[0])<end
    press(180,257);assert abs(int(values('solco')['out'].split('/')[0])-end)<=1
    press(276,205);assert values('solco')['active']=='0/0'
    press(84,153);assert values('solco')['playing']=='0/0'
    press(180,153);time.sleep(.4) # CUE restores zero/clears loop.
    final=values('solco')
    assert final['empty']==final['errors']==final['clips']==final['late']=='0',final
    assert final['volume']==initial['volume'],'Unexpected master volume change'
    run('studio-motion 2' if board=='amoled' else 'studio-motion 1');time.sleep(1.2)
    state=values('studio');assert state['motion_errors']=='0'
    run('studio-motion 0');run('studio-event 7 0 0');run('studio-view 1')
    print('PASS',port,'touch menus/pads/notes, tempo, scratch reversal, >3s SD loop wraps, half/double/exit, audio counters',final)
finally:
    try:run('touch 0 0 0');run('studio-motion 0')
    except Exception:pass
    p.close();(root/'progress-files'/f'solco-{board}-dev8-acceptance.json').write_text(json.dumps(history,indent=2),encoding='utf-8')
