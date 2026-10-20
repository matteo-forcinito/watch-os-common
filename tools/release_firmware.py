"""Build and publish firmware using the authenticated official gh CLI.

No OAuth/PAT values are read, copied, printed, or stored by this script.
Private source Actions or verified local production builds; the CLI verifies
packages and publishes binaries and the public OTA catalog.
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
            'amoled': 'amoled_watch', 'knob': 'knob_os', 'watcher': 'watcher_os', 'guition4848': 'guition4848_panel'}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', required=True)
    p.add_argument('--product', choices=PRODUCTS, required=True)
    p.add_argument('--version', required=True)
    p.add_argument('--project', type=Path, required=True)
    p.add_argument('--source-ref', default='main')
    p.add_argument('--run-id', type=int)
    p.add_argument('--local-build', type=Path, help='Verified production ESP-IDF build instead of Actions')
    p.add_argument('--verify-only', action='store_true', help='Prepare and verify packages without publishing')
    p.add_argument('--gh', default='gh')
    p.add_argument('--output', type=Path, default=Path('release-output'))
    p.add_argument('--timeout-minutes', type=int, default=60)
    a = p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', a.repo): p.error('Invalid source repository')
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?', a.version): p.error('Invalid version')
    executable = shutil.which(a.gh)
    if executable is None and Path(a.gh).is_file(): executable = str(Path(a.gh).resolve())
    if executable is None: p.error('Official gh CLI executable was not found')
    a.gh = str(Path(executable).resolve())
    public = a.repo.split('/')[0] + '/watch-firmware-releases'
    project = a.project.resolve()
    notes = project / 'release/notes' / (a.version + '.md')
    if not notes.is_file(): p.error('Release notes are missing')
    # First line of the notes: the short text the watch shows in "All versions".
    summary = next((l.strip() for l in notes.read_text(encoding='utf-8').splitlines() if l.strip()), '')[:79]
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
    shared_commit = None
    if a.local_build:
        build = a.local_build.resolve()
        def git(*args):
            return subprocess.check_output(['git', '-C', str(project), *args], text=True).strip()
        if git('status', '--porcelain', '--untracked-files=no'):
            raise RuntimeError('Tracked source must be clean before packaging')
        source_sha = git('rev-parse', 'HEAD')
        remote_sha = api('repos/'+a.repo+'/commits/'+a.source_ref)['sha']
        if source_sha != remote_sha: raise RuntimeError('Build source is not the published source revision')
        shared_commit = git('rev-parse', 'HEAD:watch-os-common')
        description = json.loads((build/'project_description.json').read_text())
        config = json.loads((build/'config/sdkconfig.json').read_text())
        shared_path = Path(description['build_component_info']['apps']['dir']).parents[1]
        shared_head = subprocess.check_output(['git','-C',str(shared_path),'rev-parse','HEAD'],text=True).strip()
        shared_dirty = subprocess.check_output(['git','-C',str(shared_path),'status','--porcelain','--untracked-files=no'],text=True).strip()
        if shared_head != shared_commit or shared_dirty:
            raise RuntimeError('Shared build source differs from the pinned clean revision')
        if config.get('WATCH_DEV_CONSOLE'): raise RuntimeError('Diagnostic console must be disabled')
        if description['project_name'] != PRODUCTS[a.product] or description['project_version'] != a.version:
            raise RuntimeError('Build description identity/version mismatch')
        packages=work/'packages-local';packages.mkdir()
        shutil.copyfile(build/(PRODUCTS[a.product]+'.bin'), packages/'firmware.bin')
        flash=json.loads((build/'flasher_args.json').read_text())
        archive_path=packages/(a.product+'-'+a.version+'-usb-flash.zip')
        prefix=a.product+'-'+a.version
        with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED) as archive:
            for offset,name in flash['flash_files'].items():
                if Path(name).name.lower() in ['nvs.bin','nvsfactory.bin','merged.bin']:
                    raise RuntimeError('Refusing destructive recovery image')
                archive.write(build/name, prefix+'/'+name.replace('\\','/'))
            archive.write(build/'flash_args', prefix+'/flash_args')
            archive.writestr(prefix+'/README.txt', 'Install Python esptool. From this folder run:\npython -m esptool --chip esp32s3 --port YOUR_PORT write-flash @flash_args\nExisting NVS and files are preserved. Do not erase flash.\n')
        (packages/'SHA256SUMS.txt').write_text(''.join(hashlib.sha256(x.read_bytes()).hexdigest()+'  '+x.name+'\n' for x in [packages/'firmware.bin',archive_path]),encoding='utf-8')
        run=None
        print('Local production build:', source_sha, flush=True)
    else:
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
        for download_attempt in range(3):
            # Fresh directories avoid consuming partially downloaded packages.
            packages = work/('packages-' + str(download_attempt)); packages.mkdir()
            result = gh('run','download',run,'-R',a.repo,'--name',expected_name,'--dir',packages,optional=True)
            if not result.returncode: break
            if download_attempt == 2: raise RuntimeError('Package download failed: ' + result.stderr.strip())
            print('Package download interrupted; retrying...', flush=True)
            time.sleep(5)
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
        'build_run':('https://github.com/'+a.repo+'/actions/runs/'+str(run)) if run else None,
        'build_type':'actions' if run else 'local', 'shared_commit':shared_commit,
        'assets':[{ 'name':x.name,'size':x.stat().st_size,'sha256':hashlib.sha256(x.read_bytes()).hexdigest()} for x in assets]}
    manifest=packages/'release-manifest.json';manifest.write_text(json.dumps(provenance,indent=2)+'\n',encoding='utf-8');assets.append(manifest)
    checks.write_text(''.join(hashlib.sha256(x.read_bytes()).hexdigest()+'  '+x.name+'\n' for x in assets),encoding='utf-8');assets.append(checks)
    if a.verify_only:
        print('VERIFIED:', packages, flush=True)
        return
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
          '--notes',summary],check=True)
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
