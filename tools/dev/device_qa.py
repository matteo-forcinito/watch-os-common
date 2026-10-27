"""Bounded local USB qualification; no inference, flashing or reset commands."""
import argparse
import json
from pathlib import Path
import re
import time

CRASH=re.compile(r'Guru Meditation|CORRUPT HEAP|assert failed|task stuck|Brownout|Rebooting|rst:0x|Stack canary',re.I)
SAMPLE=re.compile(r'@QA version=(\S+) free=(\d+) largest=(\d+) minimum=(\d+) psram=(\d+) integrity=([01])')


class TelemetryUnavailable(RuntimeError):pass


def sample(lines,version):
    if any(CRASH.search(x) for x in lines):raise RuntimeError('Crash/reset detected')
    if any('@ERR' in x for x in lines):raise RuntimeError('Diagnostic command rejected')
    matches=[SAMPLE.search(x) for x in lines];matches=[x for x in matches if x]
    if len(matches)!=1:raise TelemetryUnavailable('Missing diagnostic telemetry; maintenance firmware required')
    m=matches[0]
    if m[1]!=version:raise RuntimeError('Installed version differs from requested version')
    data=dict(zip(('free','largest','minimum','psram','integrity'),map(int,m.groups()[1:])))
    if not data['integrity']:raise RuntimeError('Heap corruption detected')
    return data


def assess(samples,min_free,min_block,max_loss):
    if len(samples)<2:return 'incomplete'
    if any(x['free']<min_free or x['largest']<min_block for x in samples):return 'failed'
    # Samples must all be taken at home after the same settling interval.
    if samples[0]['free']-samples[-1]['free']>max_loss:return 'needs_review'
    return 'passed'


def exercise(command,cycles,settle,version,pause=time.sleep):
    result=[]
    def read():
        data=sample(command('qa-memory'),version);result.append(data)
        return data
    def state(expected):
        lines=command('apps')
        if '@CURRENT '+expected not in lines:raise RuntimeError('UI did not reach '+expected)
    command('home');pause(settle);state('home');read()  # warm up before baseline
    for _ in range(cycles):
        command('open agentdeck');pause(settle);state('agentdeck')
        read()  # include live-app pressure, keep separate from home leak comparison
        command('home');pause(settle);state('home');read()
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',required=True);p.add_argument('--version',required=True)
    p.add_argument('--cycles',type=int,default=20);p.add_argument('--settle',type=float,default=3)
    p.add_argument('--min-free',type=int,default=30720);p.add_argument('--min-block',type=int,default=16384)
    p.add_argument('--max-loss',type=int,default=8192);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if not 1<=a.cycles<=1000 or not 1<=a.settle<=60 or min(a.min_free,a.min_block,a.max_loss)<0:p.error('Invalid bounded test settings')
    # Import lazily: parser/assessment tests do not need pyserial.
    report=dict(status='incomplete',version=a.version,port=a.port,cycles=a.cycles,started=time.time(),
                samples=[],log=[],hardware='USB console observed',scope='Agent Deck lifecycle and allocated heap integrity; not a full physical RAM test',
                thresholds=dict(min_free=a.min_free,min_block=a.min_block,max_loss=a.max_loss))
    def save():
        a.output.parent.mkdir(parents=True,exist_ok=True)
        a.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    save()
    try:
        import serial
        port=serial.Serial(port=None,baudrate=115200,timeout=.25,write_timeout=3)
        port.port=a.port;port.dtr=False;port.rts=False
        with port:
            def command(text):
                port.write((text+'\n').encode());lines=[];deadline=time.monotonic()+15
                while time.monotonic()<deadline:
                    line=port.readline(2048).decode('utf-8','replace').strip()
                    if not line:continue
                    report['log'].append(line);lines.append(line)
                    if CRASH.search(line) or line.startswith('@ERR'):raise RuntimeError('Crash/reset or rejected command: '+line)
                    if line=='@OK '+text.split()[0]:return lines
                raise TimeoutError('Console acknowledgement missing: '+text)
            def capture_command(text):
                lines=command(text)
                if text=='qa-memory':
                    report['samples'].append(sample(lines,a.version));save()
                return lines
            exercise(capture_command,a.cycles,a.settle,a.version)
        all_samples=report['samples'];home=all_samples[::2]
        report['status']=assess(home,a.min_free,a.min_block,a.max_loss)
        if any(x['free']<a.min_free or x['largest']<a.min_block for x in all_samples):report['status']='failed'
        report['home_free_loss_bytes']=home[0]['free']-home[-1]['free']
    except (Exception,KeyboardInterrupt) as exc:
        report['status']='failed' if isinstance(exc,RuntimeError) and not isinstance(exc,TelemetryUnavailable) else 'incomplete'
        report['error']=str(exc)
    finally:
        report['finished']=time.time();save()
    print(json.dumps(dict(status=report['status'],report=str(a.output)),indent=2))
    return 0 if report['status']=='passed' else 1

if __name__=='__main__':raise SystemExit(main())
