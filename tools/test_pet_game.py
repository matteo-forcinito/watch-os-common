"""Compile and exercise the actual game simulation with MSVC, without a device."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
work = Path(tempfile.mkdtemp(prefix='pet-games-'))
vswhere = Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
vs = Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-property', 'installationPath'], text=True).strip())
setup = vs/'Common7/Tools/VsDevCmd.bat'
raw = subprocess.check_output(f'cmd.exe /d /s /c ""{setup}" -arch=x64 >nul && set"', text=True)
env = dict(os.environ)
for line in raw.splitlines():
    if '=' in line and not line.startswith('='):
        k, v = line.split('=', 1)
        env[k.upper()] = v
compiler = sorted((vs/'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/cl.exe'))[-1]
subprocess.run([str(compiler), '/nologo', '/std:c11', '/W4', '/WX',
                '/I'+str(root/'components/apps/pet'), str(root/'tools/test_pet_game.c'),
                str(root/'components/apps/pet/pet_game.c'), '/Fe:'+str(work/'test.exe')],
               cwd=work, env=env, check=True)
subprocess.run([str(work/'test.exe')], check=True)
