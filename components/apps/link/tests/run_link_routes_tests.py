from pathlib import Path
import subprocess
import tempfile

test_dir=Path(__file__).resolve().parent
module_dir=test_dir.parent
repo_root=test_dir.parents[4]
candidates=[repo_root/board/'managed_components/espressif__cjson/cJSON' for board in ('ESP32-S3-Amoled-Touch-1.8','ESP32-S3-Touch-AMOLED-1.75C')]
cjson=next((p for p in candidates if (p/'cJSON.c').is_file()),None)
if cjson is None: raise SystemExit('Real ESP-IDF cJSON.c not found')
with tempfile.TemporaryDirectory(prefix='link-routes-') as td:
    td=Path(td); exe=td/'routes.exe'; obj=td/'obj'; obj.mkdir(); batch=td/'run.cmd'
    cmd=(f'call "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat" >nul && '
         f'cl /nologo /std:c11 /W3 /D CJSON_NESTING_LIMIT=1000 /I"{test_dir/"routes_stubs"}" /I"{module_dir}" /I"{cjson}" '
         f'"{test_dir/"test_link_routes.c"}" "{module_dir/"link_routes.c"}" "{cjson/"cJSON.c"}" '
         f'/Fo:"{obj.as_posix()}/" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"')
    batch.write_text('@echo off\n'+cmd+'\n',encoding='utf-8')
    subprocess.run(['cmd.exe','/d','/c',str(batch)],cwd=test_dir,check=True)
