"""Compile and run the pure-C Wi-Fi channel policy tests with MSVC."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--vcvars', type=Path, default=Path(
    'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
args = parser.parse_args()
test_dir = Path(__file__).resolve().parent

with tempfile.TemporaryDirectory(prefix='wifi-channel-') as tmp:
    tmp = Path(tmp)
    exe = tmp / 'wifi_channel_tests.exe'
    batch = tmp / 'run.cmd'
    command = (
        f'call "{args.vcvars}" >nul && '
        f'cl /nologo /std:c11 /W4 /WX /I"{test_dir.parent}" '
        f'"{test_dir / "test_wifi_channel.c"}" /Fe:"{exe}" '
        f'/link /OUT:"{exe}" && "{exe}"'
    )
    batch.write_text('@echo off\n' + command + '\n', encoding='utf-8')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=test_dir, check=True)
