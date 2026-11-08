"""Test the production large-command client with a bounded fake family RPC."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--vcvars', type=Path, default=Path(
    'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
args = parser.parse_args()
tests = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='orbit-large-') as temp:
    temp = Path(temp)
    exe = temp / 'large_tests.exe'
    batch = temp / 'run.cmd'
    batch.write_text(
        '@echo off\n'
        f'call "{args.vcvars}" >nul && cl /nologo /std:c11 /experimental:c11atomics /W3 '
        f'/I"{tests / "large_stubs"}" /I"{tests.parents[1] / "include"}" '
        f'"{tests / "test_orbit_large.c"}" "{tests.parent / "ecos_orbit_large.c"}" '
        f'/Fo:"{temp.as_posix()}/" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"\n', encoding='utf-8')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=tests, check=True)
