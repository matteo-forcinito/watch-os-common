"""Compile production bounded RPC reassembly with MSVC."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--vcvars', type=Path, default=Path(
    'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
parser.add_argument('--test', default='test_orbit_fragment.c', choices=['test_orbit_fragment.c', 'test_stream_slot.c'])
args = parser.parse_args()
tests = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='orbit-fragment-') as temp:
    temp = Path(temp)
    exe = temp / 'fragment_tests.exe'
    batch = temp / 'run.cmd'
    batch.write_text(
        '@echo off\n'
        f'call "{args.vcvars}" >nul && cl /nologo /std:c11 /W3 '
        f'"{tests / args.test}" /Fo:"{temp / "fragment.obj"}" '
        f'/Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"\n', encoding='utf-8')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=tests, check=True)
