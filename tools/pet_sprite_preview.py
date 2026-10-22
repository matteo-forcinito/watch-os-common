"""Compile the real C pet sprites on Windows (MSVC), check determinism and write
build-preview/pet_sprites.png: one row per species and growth, poses in columns."""
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

root = Path(__file__).resolve().parents[1]
work = Path(tempfile.mkdtemp(prefix='pet-sprite-'))
(work / 'main.c').write_text(r'''
#include "orbi/pet_sprite.h"
#include <stdio.h>
int main(void){
    uint8_t g[PET_GRID*PET_GRID], h[PET_GRID*PET_GRID]; uint32_t pal[PET_COLORS];
    for(int sp=0;sp<PET_SPECIES;sp++) for(int lv=0;lv<3;lv++){
        static const int LV[3]={1,6,15};
        uint64_t seed=0; pet_look_t l;
        for(uint64_t s=1;;s++){ pet_look(s*0x9E3779B97F4A7C15ULL,LV[lv],&l); if(l.species==sp){seed=s*0x9E3779B97F4A7C15ULL;break;} }
        pet_look(seed,LV[lv],&l); pet_palette(&l,0,pal);
        for(int pose=0;pose<6;pose++) for(int f=0;f<2;f++){
            pet_draw(&l,(pet_pose_t)pose,(uint32_t)f,0,g); pet_draw(&l,(pet_pose_t)pose,(uint32_t)f,0,h);
            for(int i=0;i<PET_GRID*PET_GRID;i++){ if(g[i]!=h[i]) return 2; printf("%08x ",pal[g[i]]); }
            printf("\n");
        }
    }
    return 0;
}
''')
vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
vs = Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-property', 'installationPath'], text=True).strip())
raw = subprocess.check_output(f'cmd.exe /d /s /c ""{vs / "Common7/Tools/VsDevCmd.bat"}" -arch=x64 >nul && set"', text=True)
env = dict(os.environ)
for line in raw.splitlines():
    if '=' in line and not line.startswith('='):
        k, v = line.split('=', 1)
        env[k.upper()] = v
cl = sorted((vs / 'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/cl.exe'))[-1]
subprocess.run([str(cl), '/nologo', '/std:c11', '/W3', '/I' + str(root / 'components/orbi/include'), 'main.c',
                str(root / 'components/orbi/pet_sprite.c'), '/Fe:sprites.exe'], env=env, cwd=work, check=True,
               stdout=subprocess.DEVNULL)
out = subprocess.run([str(work / 'sprites.exe')], capture_output=True, text=True, check=True).stdout.split('\n')
frames = [[int(v, 16) for v in line.split()] for line in out if line.strip()]
S, G, COLS = 4, 32, 12
rows = len(frames) // COLS
W, H = COLS * (G * S + 4), rows * (G * S + 4)
img = bytearray(b'\x30\x38\x48' * W * H)
for n, fr in enumerate(frames):
    ox, oy = (n % COLS) * (G * S + 4), (n // COLS) * (G * S + 4)
    for i, c in enumerate(fr):
        if c >> 24 == 0:
            continue
        a = (c >> 24) / 255
        for dy in range(S):
            for dx in range(S):
                p = ((oy + (i // G) * S + dy) * W + ox + (i % G) * S + dx) * 3
                for k, sh in enumerate((16, 8, 0)):
                    img[p + k] = int(img[p + k] * (1 - a) + ((c >> sh) & 255) * a)
raw = b''.join(b'\0' + bytes(img[y * W * 3:(y + 1) * W * 3]) for y in range(H))
chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
target = root / 'build-preview/pet_sprites.png'
target.parent.mkdir(exist_ok=True)
target.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
                   chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
print('Pet sprites deterministic; preview:', target)
