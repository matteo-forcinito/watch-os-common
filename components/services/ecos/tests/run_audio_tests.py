"""Verify the shared IMA codec through Orbit packets and the legacy Solco test."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--vcvars', type=Path, default=Path(
    'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
args = parser.parse_args()
tests = Path(__file__).resolve().parent
repo = tests.parents[3]
with tempfile.TemporaryDirectory(prefix='orbit-audio-') as temp:
    temp = Path(temp)
    for source, name in ((tests / 'test_audio_wire.c', 'orbit'), (repo / 'solco/tests/test_adpcm.c', 'solco')):
        exe = temp / f'{name}.exe'
        batch = temp / f'{name}.cmd'
        batch.write_text(
            '@echo off\n'
            f'call "{args.vcvars}" >nul && cl /nologo /std:c11 /W3 '
            f'/I"{repo / "components/core/include"}" /I"{repo / "solco/main"}" '
            f'"{source}" /Fo:"{temp / (name + ".obj")}" /Fe:"{exe}" '
            f'/link /OUT:"{exe}" && "{exe}"\n', encoding='utf-8')
        subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=tests, check=True)
