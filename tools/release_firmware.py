"""Build and publish firmware using the authenticated official gh CLI.

No OAuth/PAT values are read, copied, printed, or stored by this script.
Private source Actions build; the CLI verifies packages and publishes binaries
and the public OTA catalog. One command works on Windows, Linux and macOS.
"""
import argparse
import base64
import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

PRODUCTS = {'amoled175': 'amoled175_watch', 'amoled175c': 'amoled175c_watch',
            'amoled': 'amoled_watch', 'knob': 'knob_os', 'watcher': 'watcher_os'}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', required=True)
    p.add_argument('--product', choices=PRODUCTS, required=True)
    p.add_argument('--version', required=True)
    p.add_argument('--project', type=Path, required=True)
    p.add_argument('--source-ref', default='main')
    p.add_argument('--run-id', type=int)
    p.add_argument('--gh', default='gh')
    p.add_argument('--output', type=Path, default=Path('release-output'))
    p.add_argument('--timeout-minutes', type=int, default=60)
    a = p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', a.repo): p.error('Invalid source repository')
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?', a.version): p.error('Invalid version')
    public = a.repo.split('/')[0] + '/watch-firmware-releases'
    project = a.project.resolve()
    notes = project / 'release/notes' / (a.version + '.md')
    if not notes.is_file(): p.error('Release notes are missing')
    a.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=a.product+'-'+a.version+'-', dir=a.output.resolve()))
    def gh(*args, optional=False):
        r = subprocess.run([a.gh, *map(str,args)], capture_output=True, text=True, encoding='utf-8')
        if r.returncode and not optional:
            raise RuntimeError('gh '+str(args[0])+': '+r.stderr.strip())
        return r
    def api(path, optional=False):
        r = gh('api',path,optional=optional)
        return None if r.returncode else json.loads(r.stdout)
    def put(path, body, method='PUT'):
        target = work / 'api-body.json'
        target.write_text(json.dumps(body), encoding='utf-8')
        return gh('api','--method',method,path,'--input',target,optional=True)
    run = a.run_id
    if run is None:
        sha = api('repos/'+a.repo+'/commits/'+a.source_ref)['sha']
        started = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(seconds=2)
        gh('workflow','run','release.yml','-R',a.repo,'--ref',a.source_ref,'--field','version='+a.version)
        for _ in range(30):
            runs = json.loads(gh('run','list','-R',a.repo,'--workflow','release.yml','--limit','20',
                '--json','databaseId,headSha,event,createdAt').stdout)
            found = [r for r in runs if r['headSha']==sha and r['event']=='workflow_dispatch' and
                     datetime.datetime.fromisoformat(r['createdAt'].replace('Z','+00:00')) >= started]
            if found: run = found[0]['databaseId']; break
            time.sleep(2)
        if run is None: raise RuntimeError('Dispatched build was not found')
    print('Build:', 'https://github.com/'+a.repo+'/actions/runs/'+str(run), flush=True)
    expected_name = a.product+'-release-'+a.version
    deadline = time.monotonic()+a.timeout_minutes*60
    while time.monotonic()<deadline:
        artifacts = api('repos/'+a.repo+'/actions/runs/'+str(run)+'/artifacts')['artifacts']
        if any(x['name']==expected_name and not x['expired'] for x in artifacts): break
        status = api('repos/'+a.repo+'/actions/runs/'+str(run))
        if status['status']=='completed': raise RuntimeError('Build ended without verified release packages: '+str(status['conclusion']))
        print('Waiting for verified packages...', flush=True)
        time.sleep(15)
    else: raise RuntimeError('Build timeout')
    status = api('repos/'+a.repo+'/actions/runs/'+str(run))
    source_sha = status['head_sha']
    packages = work/'packages'; packages.mkdir()
    gh('run','download',run,'-R',a.repo,'--name',expected_name,'--dir',packages)
    firmware = packages/'firmware.bin'
    data = firmware.read_bytes()
    if len(data)<256 or struct.unpack_from('<I',data,32)[0]!=0xABCD5432: raise RuntimeError('Invalid ESP application image')
    def field(start): return data[start:start+32].split(b'\0',1)[0].decode('ascii')
    if field(48)!=a.version or field(80)!=PRODUCTS[a.product]: raise RuntimeError('Wrong firmware version or hardware identity')
    checks = packages/'SHA256SUMS.txt'
    for line in checks.read_text(encoding='utf-8').splitlines():
        digest,name=line.split(maxsplit=1); target=packages/Path(name.lstrip('*').replace('\\','/')).name
        if not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest()!=digest.lower(): raise RuntimeError('Checksum mismatch: '+target.name)
    zips=list(packages.glob('*.zip'))
    if not any('usb-flash' in z.name for z in zips): raise RuntimeError('USB recovery package missing')
    for z in zips:
        with zipfile.ZipFile(z) as archive:
            if archive.testzip() is not None: raise RuntimeError('Damaged package: '+z.name)
            if 'usb-flash' in z.name:
                names=archive.namelist()
                if not any(n.endswith('/'+PRODUCTS[a.product]+'.bin') for n in names): raise RuntimeError('Wrong USB package identity')
                if any(Path(n).name.lower() in ['nvs.bin','nvsfactory.bin','merged.bin'] for n in names): raise RuntimeError('Unsafe USB recovery package')
    assets=[firmware,*zips]
    provenance={'source_repository':a.repo,'source_commit':source_sha,'product':a.product,'version':a.version,
        'build_run':'https://github.com/'+a.repo+'/actions/runs/'+str(run),
        'assets':[{ 'name':x.name,'size':x.stat().st_size,'sha256':hashlib.sha256(x.read_bytes()).hexdigest()} for x in assets]}
    manifest=packages/'release-manifest.json';manifest.write_text(json.dumps(provenance,indent=2)+'\n',encoding='utf-8');assets.append(manifest)
    checks.write_text(''.join(hashlib.sha256(x.read_bytes()).hexdigest()+'  '+x.name+'\n' for x in assets),encoding='utf-8');assets.append(checks)
    tag=a.product+'-v'+a.version
    existing=gh('release','view',tag,'-R',public,'--json','isDraft',optional=True)
    if not existing.returncode and not json.loads(existing.stdout)['isDraft']: raise RuntimeError('Published versions are immutable: '+tag)
    if existing.returncode:
        args=['release','create',tag,'-R',public,'--target','main','--title',a.product+' '+a.version,'--notes-file',notes,'--draft']
        if '-' in a.version: args.append('--prerelease')
        gh(*args)
    gh('release','upload',tag,'-R',public,*assets,'--clobber')
    # Tag the exact private source revision; never rewrite a different tag.
    old=api('repos/'+a.repo+'/git/ref/tags/'+tag,optional=True)
    if old is not None and old['object']['sha']!=source_sha: raise RuntimeError('Source tag already points to a different commit')
    if old is None:
        r=put('repos/'+a.repo+'/git/refs',{'ref':'refs/tags/'+tag,'sha':source_sha},'POST')
        if r.returncode: raise RuntimeError(r.stderr)
    # All verified assets are uploaded before publishing. OTA catalog follows.
    gh('release','edit',tag,'-R',public,'--draft=false')
    catalog_path='repos/'+public+'/contents/'+a.product+'.json'
    catalog_tool=project/'tools/update_catalog.py'
    if not catalog_tool.is_file(): raise RuntimeError('Catalog updater missing')
    for attempt in range(5):
        current=api(catalog_path,optional=True)
        catalog=work/(a.product+'.json')
        catalog.write_bytes(base64.b64decode(current['content']) if current else b'{}')
        subprocess.run([sys.executable,str(catalog_tool),str(catalog),'--version',a.version,
          '--url','https://github.com/'+public+'/releases/download/'+tag+'/firmware.bin',
          '--size',str(len(data)),'--sha256',hashlib.sha256(data).hexdigest(),
          '--notes','Gestione diretta, mirroring opzionale ed Ecosystem.'],check=True)
        body={'message':a.product+' '+a.version,'content':base64.b64encode(catalog.read_bytes()).decode('ascii')}
        if current: body['sha']=current['sha']
        result=put(catalog_path,body)
        if not result.returncode: break
        if attempt==4: raise RuntimeError('Release published but catalog update failed: '+result.stderr)
        time.sleep(2)
    url='https://github.com/'+public+'/releases/tag/'+tag
    (work/'published.json').write_text(json.dumps({'url':url,**provenance},indent=2),encoding='utf-8')
    print('PUBLISHED:',url,flush=True)

if __name__=='__main__': main()
