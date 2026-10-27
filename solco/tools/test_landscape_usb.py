"""Muted hardware acceptance for Solco's landscape touch layout."""
import contextlib,importlib.util,io,json,re,sys,time
from pathlib import Path
import serial
root=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('devcon',root/'ESP32-S3-Knob-Touch-LCD-1.8/tools/devcon.py')
d=importlib.util.module_from_spec(spec);spec.loader.exec_module(d)
port_name,output=sys.argv[1:];history=[];report={'status':'running','history':history}
p=serial.Serial();p.port=port_name;p.baudrate=115200;p.timeout=.25;p.write_timeout=3;p.dtr=p.rts=False

def save():Path(output).write_text(json.dumps(report,indent=2),encoding='utf-8')
def run(command):
 text=io.StringIO()
 with contextlib.redirect_stdout(text):lines=d.run(p,command,timeout=20)
 entry={'command':command,'lines':lines,'log':text.getvalue()};history.append(entry);save()
 assert not any(x.startswith('@ERR') for x in lines),entry
 assert not re.search('Guru Meditation|task stuck|CORRUPT HEAP',entry['log']),entry
 assert any(x.startswith('@OK') for x in lines),'Missing acknowledgement: '+command
 return '\n'.join(lines)
def fields(command,marker):
 text=run(command);line=next(x for x in text.splitlines() if x.startswith(marker+' '))
 return dict(re.findall(r'(\w+)=([^ ]+)',line))
def layout(**expected):
 result=fields('solco-layout','@LAYOUT')
 for key,value in expected.items():assert result[key]==str(value),(key,result)
 return result
def press(x,y):run(f'press {x*448//360} {y*368//360} 110');time.sleep(.3)
def muted():assert fields('solco','@SOLCO')['volume']=='0','User volume changed; stop test'

save()
try:
 p.open();time.sleep(.5)
 muted();layout(width=448,height=368,landscape=1,studio=-1,detail=0)
 press(180,20);layout(studio=0)
 press(180,158);layout(studio=1)
 before=int(fields('studio','@STUDIO')['hits'])
 for i in range(8):press(49+(i%4)*87,124+(i//4)*108)
 after=int(fields('studio','@STUDIO')['hits']);assert after-before==8,(before,after)
 run('studio-view -1');layout(studio=-1)
 for deck in range(2):
  press(51+deck*176,93);layout(detail=1,deck=deck)
  press(40,331);assert fields('solco','@SOLCO')['playing'].split('/')[deck]=='1'
  press(40,331);assert fields('solco','@SOLCO')['playing'].split('/')[deck]=='0'
  press(320,29);assert fields('studio','@UI')['deck_menu']=='1'
  press(239,53);assert fields('studio','@UI')['deck_menu']=='0'
  press(40,29);layout(detail=0)
 press(144,184);layout(library=1)
 press(62,64);layout(library=0)
 memory=fields('qa-memory','@QA');assert memory['integrity']=='1'
 assert memory['version']=='0.1.0-dev.10',memory
 muted();report.update(status='passed',pad_hits=after-before,memory=memory,audio=fields('solco','@SOLCO'))
except Exception as exc:
 report.update(status='failed',error=str(exc));raise
finally:
 report['finished']=time.time();save();p.close()
print(json.dumps({k:v for k,v in report.items() if k!='history'},indent=2))
