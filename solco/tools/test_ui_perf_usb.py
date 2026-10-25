"""Measure UI passes during full-screen platter entry and repeated reversals."""
import contextlib,io,importlib.util,re,sys,time
from pathlib import Path
import serial
port,board=sys.argv[1:]
sp=importlib.util.spec_from_file_location('d',Path('ESP32-S3-Knob-Touch-LCD-1.8/tools/devcon.py'));d=importlib.util.module_from_spec(sp);sp.loader.exec_module(d)
p=serial.Serial();p.port=port;p.baudrate=115200;p.timeout=.2;p.write_timeout=3;p.dtr=p.rts=False;p.open()
def run(cmd,delay=.15):
    output=io.StringIO()
    with contextlib.redirect_stdout(output):lines=d.run(p,cmd,timeout=3)
    print(cmd,lines,output.getvalue());time.sleep(delay)
    return output.getvalue()+'\n'+'\n'.join(lines)
def touch(x,y,on):
    if board=='amoled':x,y=x*368//360,y*448//360
    elif board=='watcher':x,y=x*412//360,y*412//360
    run(f'touch {x} {y} {on}',.05)
try:
    run('loglevel 3');run('health');run('deck-open 0',.5);entry=run('health')
    touch(300,180,1)
    for i in range(12):touch(300,230 if i%2==0 else 180,1)
    touch(300,180,0);result=run('health');run('loglevel 2');run('studio-view 1')
    matches=re.findall(r'slowest UI pass (\d+) ms',result)
    assert matches,'No UI timing captured'
    measured=int(matches[-1]);assert measured<100,measured
    entered=re.findall(r'slowest UI pass (\d+) ms',entry)
    print('PASS',port,'steady scratch UI maximum',measured,'ms; entry maximum',entered[-1] if entered else 'unavailable','ms')
finally:
    try:run('touch 0 0 0')
    except Exception:pass
    p.close()
