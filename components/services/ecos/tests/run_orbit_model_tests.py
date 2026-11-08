"""Compile and run the pure-C ORBIT model tests with MSVC."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--vcvars', type=Path, default=Path(
    'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
args = parser.parse_args()
test_dir = Path(__file__).resolve().parent
source = test_dir / 'test_orbit_model.c'

with tempfile.TemporaryDirectory(prefix='ecos-orbit-model-') as temp:
    temp = Path(temp)
    exe = temp / 'ecos_orbit_model_tests.exe'
    obj_dir = temp / 'obj'
    obj_dir.mkdir()
    batch = temp / 'run.cmd'
    command = (
        f'call "{args.vcvars}" >nul && '
        f'cl /nologo /std:c11 /W4 /I"{test_dir}" '
        f'/Fo:"{obj_dir.as_posix()}/" /Fe:"{exe}" '
        f'"{source}" /link /OUT:"{exe}" && "{exe}"'
    )
    batch.write_text('@echo off\n' + command + '\n', encoding='utf-8')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=test_dir, check=True)
