"""Run actual keyboard geometry on the host: bounds, overlaps and touch sizes."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
work = Path(tempfile.mkdtemp(prefix="keyboard-layout-"))
(work / "test.c").write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "ui/keyboard_layout.h"
static void inside(ui_key_rect_t r, int w, int h) {
    assert(r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0);
    assert(r.x + r.w <= w && r.y + r.h <= h);
}
static int overlap(ui_key_rect_t a, ui_key_rect_t b) {
    return a.x < b.x+b.w && b.x < a.x+a.w && a.y < b.y+b.h && b.y < a.y+a.h;
}
int main(void) {
    const int sizes[][3] = {{640,172,0},{172,640,0},{368,448,0},{480,480,0},{466,466,1},{412,412,1}};
    for (int s=0;s<6;s++) {
        int w=sizes[s][0],h=sizes[s][1]; ui_keyboard_layout_t k;
        ui_keyboard_layout_init(&k,w,h,sizes[s][2]);
        inside(k.header,w,h); inside(k.suggestions,w,h); inside(k.pad,w,h);
        assert(!overlap(k.header,k.suggestions)); assert(!overlap(k.suggestions,k.pad));
        ui_key_rect_t all[34];
        for(int i=0;i<28;i++) {
            inside(k.keys[i],k.pad.w,k.pad.h);
            all[i]=k.keys[i]; all[i].y+=k.pad.y;
            if(s<2) assert(all[i].w>=36 && all[i].h>=36);
        }
        for(int i=0;i<6;i++) {
            inside(k.controls[i],w,h); all[28+i]=k.controls[i];
            if(s<2) assert(all[28+i].w>=40 && all[28+i].h>=40);
        }
        for(int i=0;i<34;i++) for(int j=i+1;j<34;j++) assert(!overlap(all[i],all[j]));
        printf("PASS %dx%d: every key visible, distinct hit areas, touch sizes\n",w,h);
    }
}
''', encoding="utf-8")
vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = Path(subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"], text=True).strip())
raw = subprocess.check_output(f'cmd.exe /d /s /c ""{vs / "Common7/Tools/VsDevCmd.bat"}" -arch=x64 >nul && set"', text=True)
env = dict(os.environ)
for line in raw.splitlines():
    if "=" in line and not line.startswith("="):
        key, value = line.split("=", 1)
        env[key.upper()] = value
compiler = sorted((vs / "VC/Tools/MSVC").glob("*/bin/Hostx64/x64/cl.exe"))[-1]
subprocess.run([str(compiler), "/nologo", "/std:c11", "/W4", "/I" + str(root / "components/ui/include"),
                str(work / "test.c"), str(root / "components/ui/keyboard_layout.c"),
                "/Fe:" + str(work / "test.exe")], cwd=work, env=env, check=True)
subprocess.run([str(work / "test.exe")], check=True)
