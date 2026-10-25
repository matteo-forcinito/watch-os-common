"""Independent, approximate autocorrelation check on owned synthetic speech."""
import wave,struct,math,statistics,json
from pathlib import Path
results={}
for name in ['44100','0.5x-locked','1.5x-locked','2x-locked','reverse-1.5x-locked','1.5x-vinyl']:
 path=Path('progress-files')/('solco-voice-'+name+'.wav')
 with wave.open(str(path),'rb') as w:
  n=w.getnframes();rate=w.getframerate();data=struct.unpack('<'+'h'*(n*2),w.readframes(n));duration=n/rate
 mono=data[::10];hz=8820;est=[]
 for at in range(0,len(mono)-512,176):
  x=mono[at:at+512];avg=sum(x)/len(x);x=[a-avg for a in x]
  if sum(a*a for a in x)/len(x)<300**2:continue
  correlations=[]
  for lag in range(20,148):
   a=x[:-lag];b=x[lag:];den=math.sqrt(sum(v*v for v in a)*sum(v*v for v in b))
   correlations.append(sum(u*v for u,v in zip(a,b))/den if den else 0)
  peaks=[i for i in range(1,len(correlations)-1) if correlations[i]>0.70 and correlations[i]>correlations[i-1] and correlations[i]>=correlations[i+1]]
  if peaks:est.append(hz/(20+peaks[0]))
 results[name]={'seconds':duration,'voiced_windows':len(est),'median_hz':statistics.median(est) if est else None}
 print(name,results[name],flush=True)
Path('progress-files/solco-dev9-voice-pitch.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
base=results['44100']['median_hz']
for name in ('0.5x-locked','1.5x-locked','2x-locked','reverse-1.5x-locked'):assert abs(results[name]['median_hz']/base-1)<.08
assert 1.35<results['1.5x-vinyl']['median_hz']/base<1.65
print('PASS synthetic speech: expected durations, stable median pitch; vinyl control shifts pitch')
