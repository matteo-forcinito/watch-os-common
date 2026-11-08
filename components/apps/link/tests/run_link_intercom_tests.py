"""Compile the production Orbit intercom worker using SDK seams and real cJSON."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--vcvars', type=Path, default=Path(
    'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
parser.add_argument('--cjson', type=Path, default=None)
args = parser.parse_args()
test_dir = Path(__file__).resolve().parent
repo_root = test_dir.parents[4]
candidates = [
    repo_root / board / 'managed_components/espressif__cjson/cJSON'
    for board in ('ESP32-S3-Amoled-Touch-1.8', 'ESP32-S3-Touch-AMOLED-1.75C')
]
cjson = args.cjson or next((p for p in candidates if (p / 'cJSON.c').is_file()), None)
if cjson is None or not (cjson / 'cJSON.c').is_file():
    raise SystemExit('Real ESP-IDF cJSON.c not found; pass --cjson')
stub_dir = test_dir / 'intercom_stubs'
core_include = repo_root / 'watch-os-common/components/core/include'
board_include = repo_root / 'watch-os-common/components/board_api/include'

with tempfile.TemporaryDirectory(prefix='link-intercom-') as temp:
    temp = Path(temp)
    exe = temp / 'link_intercom_tests.exe'
    obj_dir = temp / 'obj'
    obj_dir.mkdir()
    batch = temp / 'run.cmd'
    command = (
        f'call "{args.vcvars}" >nul && '
        f'cl /nologo /std:c11 /W3 /D CJSON_NESTING_LIMIT=1000 '
        f'/I"{stub_dir}" /I"{test_dir.parent}" /I"{core_include}" '
        f'/I"{board_include}" /I"{cjson}" '
        f'"{test_dir / "test_link_intercom.c"}" "{cjson / "cJSON.c"}" '
        f'/Fo:"{obj_dir.as_posix()}/" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"'
    )
    batch.write_text('@echo off\n' + command + '\n', encoding='utf-8')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=test_dir, check=True)
