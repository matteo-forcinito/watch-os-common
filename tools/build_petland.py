"""Build the external RPG without modifying a board or the OS build cache.
Run in an ESP-IDF PowerShell environment. Output includes an SD app folder.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--board', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--prepare-only', action='store_true')
parser.add_argument('--pokemon-assets', type=Path, help='Generated personal-use SpriteCollab pack directory')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
board, output = args.board.resolve(), args.output.resolve()
if not (board/'components/hardware').is_dir():
    parser.error('The board must contain components/hardware')
defaults = next((board/'external_apps'/name/'sdkconfig.defaults' for name in ['anello', 'remote', 'casa', 'motore']
                 if (board/'external_apps'/name/'sdkconfig.defaults').is_file()), board/'sdkconfig.defaults')
if not defaults.is_file():
    parser.error('No board sdkconfig.defaults found')
project = output/'project'
(project/'main').mkdir(parents=True, exist_ok=True)
for name in ['CMakeLists.txt', 'partitions.csv', 'main/CMakeLists.txt', 'main/idf_component.yml']:
    shutil.copyfile(root/'petland'/name, project/name)
shutil.copyfile(defaults, project/'sdkconfig.defaults')
if args.prepare_only:
    print(project)
    raise SystemExit(0)
env = dict(os.environ, WATCH_OS_COMMON_DIR=root.as_posix(), WATCH_BOARD_PROJECT=board.as_posix(), IDF_CCACHE_ENABLE='1')
if args.pokemon_assets:
    assets = args.pokemon_assets.resolve()
    if not (assets/'pokemon_assets.c').is_file() or not (assets/'pokemon_assets.h').is_file():
        parser.error('Run tools/prepare_pokemon.py first')
    env['PETLAND_POKEMON_ASSETS'] = assets.as_posix()
subprocess.run(['cmake', '-G', 'Ninja', '-S', str(project), '-B', str(output/'build')], env=env, check=True)
subprocess.run(['cmake', '--build', str(output/'build'), '--parallel', '2'], env=env, check=True)
data = (output/'build/petland.bin').read_bytes()
assert data[48:80].split(b'\0')[0] == b'1.0.0'
assert data[80:112].split(b'\0')[0] == b'petland'
package = output/'sdcard/apps/petland'
package.mkdir(parents=True, exist_ok=True)
(package/'Petland.bin').write_bytes(data)
(package/'manifest.json').write_text(json.dumps({'name':'Petland', 'bin':'Petland.bin', 'version':'1.0.0'}), encoding='utf-8')
if env.get('PETLAND_POKEMON_ASSETS'):
    credits = Path(env['PETLAND_POKEMON_ASSETS'])/'CREDITS.json'
    if credits.is_file(): shutil.copyfile(credits, package/'CREDITS.json')
(output/'build-info.json').write_text(json.dumps({'board':board.name, 'version':'1.0.0',
    'sha256':hashlib.sha256(data).hexdigest(), 'size':len(data)}, indent=2), encoding='utf-8')
print('Petland ready:', package)
