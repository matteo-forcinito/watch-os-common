"""Build no-LVGL Watch Lite for one or more supported board projects.

Run from an activated ESP-IDF 6.1 terminal:
  python tools/build_lite.py ../ESP32-S3-Touch-AMOLED-1.75C --output ../lite-builds
Never flash the generated partition table over an existing installation.
Install only watch_lite.bin using the external-app installer.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('boards', type=Path, nargs='+')
p.add_argument('--output', type=Path, default=root/'lite/builds')
p.add_argument('--qa', action='store_true', help='Local tests only: starts Wi-Fi and logs the pairing code')
a = p.parse_args()
idf = Path(os.environ['IDF_PATH'])/'tools/idf.py'
for board in a.boards:
    board = board.resolve()
    if not (board/'components/hardware/CMakeLists.txt').is_file():
        p.error('Not a supported board project: '+str(board))
    project = a.output.resolve()/board.name
    project.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root/'lite/CMakeLists.txt', project/'CMakeLists.txt')
    defaults = (board/'sdkconfig.defaults').read_text(encoding='utf-8')+'\n'
    defaults += (root/'lite/sdkconfig.defaults').read_text(encoding='utf-8')
    defaults += '\nCONFIG_PARTITION_TABLE_CUSTOM_FILENAME="'+(board/'partitions.csv').as_posix()+'"\n'
    for filename in ['sdkconfig.defaults','sdkconfig']:
        (project/filename).write_text(defaults, encoding='utf-8')
    env = dict(os.environ, WATCH_OS_COMMON_DIR=str(root), LITE_BOARD_DIR=str(board), LITE_QA='1' if a.qa else '0')
    subprocess.run([sys.executable, str(idf), '-B', 'build', 'build'], cwd=project, env=env, check=True)
    build = project/'build'
    desc = json.loads((build/'project_description.json').read_text(encoding='utf-8'))
    commands = json.loads((build/'compile_commands.json').read_text(encoding='utf-8'))
    if any('lvgl' in str(component).lower() for component in desc['build_components']):
        raise RuntimeError('LVGL entered Lite dependency closure')
    if 'lv_init' in (build/'watch_lite.map').read_text(encoding='utf-8'):
        raise RuntimeError('LVGL entered Lite linker map')
    qa = any('-DLITE_QA=1' in c['command'] for c in commands)
    if qa != a.qa:
        raise RuntimeError('Unexpected QA compilation flags')
    binary = build/'watch_lite.bin'
    metadata = dict(board=board.name, project='watch_lite', version=desc['project_version'],
                    qa=qa, lvgl=False, size=binary.stat().st_size,
                    sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    (project/'lite-build.json').write_text(json.dumps(metadata,indent=2)+'\n', encoding='utf-8')
    print(('LOCAL QA' if qa else 'PRODUCTION')+' LITE VERIFIED: '+str(binary), flush=True)
