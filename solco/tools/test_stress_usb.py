"""Cold-import plus two-SD-deck stress. Audible runs require an explicit flag."""
import importlib.util
import json
from pathlib import Path
import re
import sys
import time
import serial

allow_volume = "--allow-user-volume" in sys.argv
args = [arg for arg in sys.argv[1:] if arg != "--allow-user-volume"]
name, helper, board, report = args
spec = importlib.util.spec_from_file_location('devcon', helper)
devcon = importlib.util.module_from_spec(spec)
spec.loader.exec_module(devcon)
port = serial.Serial()
port.port, port.baudrate, port.timeout, port.write_timeout = name, 115200, .3, 3
port.dtr = port.rts = False
port.open()
time.sleep(.5)
history = []
play = [(130,139),(130,256)] if board == 'amoled' else [(143,115),(143,203)]
cue = [(219,139),(219,256)] if board == 'amoled' else [(214,115),(214,203)]

def run(command):
    port.write(b'\n');port.flush()
    response = devcon.run(port, command, timeout=2)
    history.append({'time':round(time.monotonic(),3),'command':command,'response':response})
    Path(report).write_text(json.dumps(history,indent=2),encoding='utf-8')
    return response

def status(allow_error=False):
    for attempt in range(4):
        lines = run('solco')
        if not allow_error and any(x.startswith('@ERROR') for x in lines): raise RuntimeError(lines)
        v = dict(re.findall(r'(\w+)=([-\d/]+)', ' '.join(lines)))
        if not allow_volume and 'volume' in v and v['volume'] != '0': raise RuntimeError('User changed volume: stop gestures')
        if {'volume','playing','ready','busy','pos','late','cycle_us'}.issubset(v):
            assert v['empty'] == v['errors'] == v['clips'] == v['late'] == '0',v
            return v
    raise RuntimeError('Incomplete USB status')

def press(point):
    before = status()
    response = run(f'press {point[0]} {point[1]} 100')
    time.sleep(.15)
    if not any('@OK press' in x for x in response):
        after = status()
        # Never blindly retry a toggle after a lost USB acknowledgement.
        changed = point in play and before['playing'].split('/')[play.index(point)] != after['playing'].split('/')[play.index(point)]
        if not changed:
            run(f'press {point[0]} {point[1]} 100')
            time.sleep(.15)

def wait_ready():
    end=time.monotonic()+5
    while time.monotonic()<end:
        v=status()
        if v['ready']=='1/1':return v
        time.sleep(.1)
    raise RuntimeError('Deck not ready')

try:
    run('loglevel 2')
    initial = status(allow_error=True)
    # Shared header position closes either detail or library, harmless on mixer.
    bx, by = (54,58) if board == 'amoled' else (90,56)
    run(f'press {bx} {by} 100')
    time.sleep(.15)
    for x,y in cue:
        run(f'press {x} {y} 100');time.sleep(.15)
    assert status()['playing']=='0/0'
    assert any('match=1' in x for x in run('solco-mp3-check'))
    run('solco-load-stress')
    time.sleep(.15)
    initial=status()
    assert initial['ready'].endswith('/1')
    assert initial['busy'].startswith('1/'),initial
    press(play[1])
    started=time.monotonic()
    saw_concurrent=False
    while time.monotonic()-started<60:
        v=status()
        if v['busy'].startswith('1/') and v['playing'].endswith('/1'):
            saw_concurrent=True
        if int(v['pos'].split('/')[1])>350000:
            press(cue[1]);time.sleep(.15);press(play[1])
        if v['ready']=='1/1' and v['busy']=='0/0':break
        time.sleep(.2)
    else:raise RuntimeError('Cold import timeout')
    assert saw_concurrent,'Cold importer was not observed with B SD playing'
    cold_seconds=round(time.monotonic()-started,2)
    dual=[]
    for cycle in range(6):
        press(cue[0]);press(cue[1]);wait_ready()
        press(play[0]);press(play[1])
        first=status();assert first['playing']=='1/1',first
        time.sleep(2)
        after=status()
        assert after['playing']=='1/1',after
        assert all(int(b)>int(a)+44100 for a,b in zip(first['pos'].split('/'),after['pos'].split('/'))),(first,after)
        dual.append(after)
    press(cue[0]);press(cue[1]);wait_ready()
    final=status();assert final['playing']=='0/0' and final['held']=='0/0',final
    print(json.dumps({'result':'PASS','board':board,'cold_seconds':cold_seconds,'concurrent_import':saw_concurrent,'dual_cycles':len(dual),'final':final},indent=2),flush=True)
finally:
    run('loglevel 3')
    port.close()
