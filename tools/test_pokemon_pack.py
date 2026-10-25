"""Exercise the real portable C reader and exporter, including malformed files."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import sys
import zlib
from export_pet_pokemon import export

root = Path(__file__).resolve().parents[1]
work = Path(tempfile.mkdtemp(prefix='pokemon-pack-'))
vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-property', 'installationPath'], text=True).strip())
setup = vs / 'Common7/Tools/VsDevCmd.bat'
raw = subprocess.check_output(f'cmd.exe /d /s /c ""{setup}" -arch=x64 >nul && set"', text=True)
env = dict(os.environ)
for line in raw.splitlines():
    if '=' in line and not line.startswith('='):
        k, v = line.split('=', 1)
        env[k.upper()] = v
compiler = sorted((vs / 'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/cl.exe'))[-1]
exe = work / 'test.exe'
subprocess.run([str(compiler), '/nologo', '/std:c11', '/W4', '/WX', '/D_CRT_SECURE_NO_WARNINGS',
                '/I' + str(root / 'components/orbi/include'), str(root / 'tools/test_pokemon_pack.c'),
                str(root / 'components/orbi/pokemon_pack.c'), '/Fe:' + str(exe)],
               cwd=work, env=env, check=True)
pixels = [255] * 800 + [0] * 800 + [1] * 1600
# Left top transparent; right top red, and second animation frame green.
pixels[39] = 0
pixels[38] = 0  # 31*40/32 samples column 38
(work / 'pokemon_assets.c').write_text(
    'static const uint16_t p25_0_pal[] = {63488,2016};\n'
    'static const uint8_t p25_0_px[] = {' + ','.join(map(str, pixels)) + '};')
(work / 'CREDITS.json').write_text('{}')
pack, count = export(work, work / 'out')
assert count == 1 and (pack.parent / 'CREDITS.json').is_file()
subprocess.run([str(exe), str(pack), 'good'], check=True)
valid = pack.read_bytes()

def bad(name, data, repair_crc=False):
    data = bytearray(data)
    if repair_crc:
        struct.pack_into('<I', data, 16, zlib.crc32(data[20:]))
    path = work / (name + '.pkp')
    path.write_bytes(data)
    subprocess.run([str(exe), str(path), 'bad'], check=True)

bad('truncated-header', valid[:10])
bad('truncated-pixels', valid[:-1])
bad('checksum', valid[:-1] + bytes([valid[-1] ^ 1]))
for name, offset, fmt, value in [
        ('magic', 0, '<I', 0), ('version', 4, '<H', 2),
        ('count', 6, '<H', 303), ('data-offset', 12, '<I', 0),
        ('dex', 20, '<H', 152), ('shiny', 22, '<B', 2),
        ('frames', 23, '<B', 0), ('offset', 24, '<I', 0xFFFFFFFF),
        ('palette', 28, '<H', 256), ('reserved', 30, '<H', 1)]:
    data = bytearray(valid)
    struct.pack_into(fmt, data, offset, value)
    bad(name, data, True)
print('POKEMON_PACK_TESTS_OK: exporter, real C decode, frame wrapping, fallback and 13 malformed packs')
if len(sys.argv) > 1:
    subprocess.run([str(exe), sys.argv[1], 'all'], check=True)
    print('POKEMON_REAL_PACK_OK: every species, variant and animation frame decoded')
