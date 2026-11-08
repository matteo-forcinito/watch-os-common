"""Compile and run the production household coordinator with real cJSON."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--vcvars', type=Path, default=Path(
    'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
parser.add_argument('--cjson', type=Path, default=None,
                    help='cJSON source directory from an ESP-IDF managed_components checkout')
parser.add_argument('--test', default='test_link_house.c', choices=['test_link_house.c', 'test_link_family.c'])
args = parser.parse_args()
test_dir = Path(__file__).resolve().parent
module_dir = test_dir.parent
repo_root = test_dir.parents[4]
candidates = [
    repo_root / board / 'managed_components/espressif__cjson/cJSON'
    for board in ('ESP32-S3-Amoled-Touch-1.8', 'ESP32-S3-Touch-AMOLED-1.75C')
]
cjson = args.cjson or next((p for p in candidates if (p / 'cJSON.c').is_file()), None)
if cjson is None or not (cjson / 'cJSON.c').is_file():
    raise SystemExit('Real ESP-IDF cJSON.c not found; pass --cjson')

with tempfile.TemporaryDirectory(prefix='link-house-') as tmp:
    tmp = Path(tmp)
    exe = tmp / 'link_house_tests.exe'
    obj_dir = tmp / 'obj'
    obj_dir.mkdir()
    batch = tmp / 'run.cmd'
    command = (
        f'call "{args.vcvars}" >nul && '
        f'cl /nologo /std:c11 /W3 /D CJSON_NESTING_LIMIT=1000 '
        f'/I"{test_dir / "house_stubs"}" /I"{module_dir}" /I"{cjson}" '
        f'"{test_dir / args.test}" "{module_dir / "link_gateway.c"}" "{cjson / "cJSON.c"}" '
        f'/Fo:"{obj_dir.as_posix()}/" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"'
    )
    batch.write_text('@echo off\n' + command + '\n', encoding='utf-8')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=test_dir, check=True)
