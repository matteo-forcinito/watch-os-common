"""Run production bus lifecycle tests on Windows with MSVC."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--vcvars', type=Path, default=Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
args = parser.parse_args()
work = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='ecos-bus-') as tmp:
    exe = Path(tmp) / 'bus_tests.exe'
    command = f'call "{args.vcvars}" >nul && cl /nologo /std:c11 /W3 /I"{work / "bus_stubs"}" "{work / "test_bus.c"}" /Fe:"{exe}" /Fo:"{Path(tmp) / "bus_tests.obj"}" /link /OUT:"{exe}" && "{exe}"'
    batch = Path(tmp) / 'run.cmd'
    batch.write_text('@echo off\n' + command + '\n')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=work, check=True)
