"""Run the standalone radio owner with deterministic MSVC SDK fault injection."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--vcvars', type=Path, default=Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
args = parser.parse_args()
tests = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='ecos-bus-standalone-') as tmp:
    exe = Path(tmp) / 'bus_standalone_tests.exe'
    obj = Path(tmp) / 'bus_standalone_tests.obj'
    batch = Path(tmp) / 'run.cmd'
    command = (f'call "{args.vcvars}" >nul && cl /nologo /std:c11 /W3 /I"{tests / "standalone_stubs"}" '
               f'"{tests / "test_bus_standalone.c"}" /Fe:"{exe}" /Fo:"{obj}" /link /OUT:"{exe}" && "{exe}"')
    batch.write_text('@echo off\n' + command + '\n')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=tests, check=True)
