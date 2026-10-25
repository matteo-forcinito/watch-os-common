"""Publish verified Solco assets and merge only its dev entries into the Market."""
import argparse,base64,hashlib,json,os,subprocess,urllib.request,urllib.error
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('directory',type=Path);p.add_argument('--publish',action='store_true');a=p.parse_args()
root=Path(__file__).resolve().parents[3];repo='matteo-forcinito/watch-firmware-releases';tag='solco-v0.1.0-dev.8'
new=json.loads((a.directory/'catalog.json').read_text(encoding='utf-8'));assert new['schema']==2
assert len(new['apps']) in (2,3) and all(x['id']=='Solco' and x['channel']=='dev' for x in new['apps'])
prefix=f'https://github.com/{repo}/releases/download/{tag}/'
assets={}
for entry in new['apps']:
 for kind in ('binary','icon'):
  item=entry[kind];assert item['url'].startswith(prefix)
  name=item['url'][len(prefix):];assert '/' not in name
  data=(a.directory/name).read_bytes();assert len(data)==item['size'] and hashlib.sha256(data).hexdigest()==item['sha256']
  assets[name]=data
assets['Solco-catalog.json']=(a.directory/'catalog.json').read_bytes()
report=root/'watch-os-common/solco/TEST-REPORT.md';assets['Solco-TEST-REPORT.md']=report.read_bytes()
packages=sorted((root/'release-output/solco/0.1.0-dev.8').glob('Solco-*.zip'))
assert len(packages)==3, 'All three board packages are required'
for z in packages:assets[z.name]=z.read_bytes()
print('Verified local assets:',len(assets),flush=True)
if not a.publish:raise SystemExit(0)
token=os.environ.get('RELEASES_TOKEN')
if not token:
 r=subprocess.run(['git','-c','safe.directory=*','credential','fill'],input='protocol=https\nhost=github.com\n\n',capture_output=True,text=True,check=True)
 token=dict(x.split('=',1) for x in r.stdout.splitlines() if '=' in x)['password']
api=f'https://api.github.com/repos/{repo}'
def req(url,method='GET',data=None,binary=False):
 assert url.startswith(('https://api.github.com/','https://uploads.github.com/'))
 headers={'Authorization':'Bearer '+token,'Accept':'application/vnd.github+json','X-GitHub-Api-Version':'2022-11-28'}
 if data is not None:
  headers['Content-Type']='application/octet-stream' if binary else 'application/json'
  if not binary:data=json.dumps(data).encode()
 with urllib.request.urlopen(urllib.request.Request(url,data=data,method=method,headers=headers),timeout=60) as response:return json.load(response)
try:release=req(api+'/releases/tags/'+tag)
except urllib.error.HTTPError as e:
 if e.code!=404:raise
 release=req(api+'/releases','POST',{'tag_name':tag,'name':'Solco 0.1.0-dev.8','draft':True,'prerelease':True,'body':
  'Build di sviluppo standalone DJ e Studio. Disco a tutto schermo e menu centrale, loop manuali IN/OUT, 8 pad e strumenti, velocita 0.5/1/1.5/2x nei due versi con transizione graduale. Velocita e intonazione restano accoppiate; nessun Master Tempo, stem separation, hub o upload BLE musicale. Nessuna radio avviata da Solco. Console USB di sviluppo attiva. Pacchetti specifici per scheda; consultare il rapporto per prove e limiti. Movimento AMOLED: stato della verifica nel rapporto allegato. Lo scrocchio segnalato sullo speaker non e dichiarato risolto.'})
uploaded={x['name']:x for x in req(api+f"/releases/{release['id']}/assets?per_page=100")}
for name,data in sorted(assets.items()):
 digest='sha256:'+hashlib.sha256(data).hexdigest()
 item=uploaded.get(name)
 if item is None:item=req(release['upload_url'].split('{')[0]+'?name='+name,'POST',data,True)
 assert item['size']==len(data) and item.get('digest')==digest,'Immutable asset mismatch: '+name
 print('Verified upload:',name,flush=True)
req(api+f"/releases/{release['id']}",'PATCH',{'draft':False,'prerelease':True,'make_latest':'false'})
path=api+'/contents/market/catalog.json'
keys={(x['id'],x['board'],x['channel']) for x in new['apps']}
for attempt in range(5):
 old=req(path+'?ref=main');previous=json.loads(base64.b64decode(old['content']))
 preserved=[x for x in previous['apps'] if (x['id'],x['board'],x['channel']) not in keys]
 merged={'schema':2,'apps':preserved+new['apps']};encoded=json.dumps(merged,ensure_ascii=False,separators=(',',':')).encode()+b'\n'
 assert len(merged['apps'])<=64 and len(encoded)<=48*1024
 for board in {x['board'] for x in merged['apps']}:assert sum(x['board']==board for x in merged['apps'])<=32
 try:
  result=req(path,'PUT',{'message':'Add Solco 0.1.0-dev.8 to Market','branch':'main','sha':old['sha'],'content':base64.b64encode(encoded).decode()});break
 except urllib.error.HTTPError as e:
  if e.code!=409 or attempt==4:raise
verified=req(path+'?ref=main');assert base64.b64decode(verified['content'])==encoded
(a.directory/'published-catalog.json').write_bytes(encoded)
receipt={'release':release['html_url'],'catalog_commit':result['commit']['sha'],'entries':len(merged['apps']),'preserved_entries':len(preserved),'assets':{n:hashlib.sha256(d).hexdigest() for n,d in assets.items()}}
(a.directory/'publication.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
print('PUBLISHED',receipt['release'],'catalog entries',receipt['entries'],'preserved',len(preserved),flush=True)
