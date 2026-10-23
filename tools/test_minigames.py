"""Test the shared engine, safe drawing, real catalog and novice generator."""
from pathlib import Path
import os
import subprocess
import tempfile
import importlib.util
import shutil

root = Path(__file__).resolve().parents[1]
work = Path(tempfile.mkdtemp(prefix='minigames-'))
spec = importlib.util.spec_from_file_location('new_minigame', root/'tools/new_minigame.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
sandbox = work/'example'
for name in ['components/apps/minigames/games/tap_star.c', 'components/apps/minigames/minigame_canvas.h',
             'components/apps/minigames/minigame.h', 'components/apps/pet/pet_game_catalog.c', 'components/apps/CMakeLists.txt']:
    target = sandbox/name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root/name, target)
generated = generator.create(sandbox, 'demo_balloons', 'Balloons')
assert 'mg_demo_balloons' in (sandbox/'components/apps/pet/pet_game_catalog.c').read_text()
assert 'minigames/games/demo_balloons.c' in (sandbox/'components/apps/CMakeLists.txt').read_text()
for name in ['demo_balloons', '../escape', 'Bad-Name', 'x'*40]:
    try:
        generator.create(sandbox, name, 'Oops')
    except ValueError:
        pass
    else:
        raise AssertionError('Generator accepted duplicate or unsafe name')

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
sources = [root/'tools/test_minigames.c', root/'components/apps/pet/pet_game.c',
           root/'components/apps/pet/pet_game_catalog.c', root/'components/apps/minigames/minigame.c',
           root/'components/apps/minigames/minigame_canvas.c', generated]
sources += sorted((root/'components/apps/minigames/games').glob('*.c'))
subprocess.run([str(compiler), '/nologo', '/std:c11', '/O2', '/W4', '/WX',
                '/I'+str(root/'components/apps/minigames'), '/I'+str(root/'components/apps/pet'),
                *map(str, sources), '/Fe:'+str(work/'test.exe')],
               cwd=work, env=env, check=True)
subprocess.run([str(work/'test.exe')], check=True)
print('PASS: beginner generator: copied game compiles, registration, duplicate and invalid-name protection')
