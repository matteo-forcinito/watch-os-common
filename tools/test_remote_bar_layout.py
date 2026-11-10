"""Exercise the actual Remote bar geometry on the host."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
work=Path(tempfile.mkdtemp(prefix="remote-bar-"))
(work / "test.c").write_text('\n#include <assert.h>\n#include <stdio.h>\n#include "remote_bar_layout.h"\nstatic int overlap(ui_key_rect_t a,ui_key_rect_t b) {\n return a.x<b.x+b.w && b.x<a.x+a.w && a.y<b.y+b.h && b.y<a.y+a.h;\n}\nint main(void) {\n const int sizes[][2]={{640,172},{800,200},{768,192},{172,640},{192,768},{200,800}};\n for(int s=0;s<6;s++) {\n  int w=sizes[s][0],h=sizes[s][1];assert(remote_bar_size(w,h)||remote_narrow_size(w,h));\n  ui_key_rect_t r[9];\n  for(int i=0;i<9;i++) {\n   r[i]=i<5?remote_bar_key(w,h,i):remote_bar_media(w,h,i-5);\n   assert(r[i].x>=0 && r[i].y>=74 && r[i].x+r[i].w<=w && r[i].y+r[i].h<=h);\n   assert(r[i].w-6>=40 && r[i].h-6>=40);\n   for(int j=0;j<i;j++)assert(!overlap(r[i],r[j]));\n  }\n }\n assert(!remote_bar_size(172,640));assert(!remote_bar_size(412,412));\n puts("Remote bar: bounds, non-overlap and 40px controls passed");\n}\n',encoding="utf-8")
vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = Path(subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"], text=True).strip())
raw = subprocess.check_output(f'cmd.exe /d /s /c ""{vs / "Common7/Tools/VsDevCmd.bat"}" -arch=x64 >nul && set"', text=True)
env = dict(os.environ)
for line in raw.splitlines():
    if "=" in line and not line.startswith("="):
        key, value = line.split("=", 1)
        env[key.upper()] = value
compiler = sorted((vs / "VC/Tools/MSVC").glob("*/bin/Hostx64/x64/cl.exe"))[-1]
subprocess.run([str(compiler),"/nologo","/std:c11","/W4","/I"+str(root/"components/ui/include"),"/I"+str(root/"remote/main"),str(work/"test.c"),"/Fe:"+str(work/"test.exe")],cwd=work,env=env,check=True)
subprocess.run([str(work/"test.exe")],check=True)
