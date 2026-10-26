"""Exercise the actual allocation-free Watch Lite navigation with MSVC."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
work=Path(tempfile.mkdtemp(prefix='lite-navigation-'))
(work/'test.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "navigation.h"
static long long at=1000000;
static lite_action_t gesture(lite_navigation_t *n,int x,int y,int ex,int ey,int duration,bool idle){
 lite_navigation_touch(n,true,x,y,at,idle,false);
 lite_navigation_touch(n,true,ex,ey,at+duration,false,false);
 lite_navigation_touch(n,false,0,0,at+duration+1000,false,false);
 lite_action_t a=lite_navigation_touch(n,false,0,0,at+duration+33000,false,false);at+=duration+100000;return a;
}
int main(void){
 const int sizes[][2]={{466,466},{368,448},{240,280},{480,480}};
 for(int s=0;s<4;s++){
  lite_navigation_t n;int w=sizes[s][0],h=sizes[s][1];lite_navigation_init(&n,w,h);
  assert(gesture(&n,w/2,h/2,w/2,h/2,120000,false)==LITE_NAV_NONE&&n.page==LITE_CLOCK);
  assert(gesture(&n,w/2,h*3/4,w/2,h/4,300000,true)==LITE_NAV_NONE&&n.page==LITE_CLOCK);
  assert(gesture(&n,w/2,h*3/4,w/2,h/4,300000,false)==LITE_NAV_CHANGED&&n.page==LITE_SETTINGS);
  int top=lite_navigation_row_top(&n),row=lite_navigation_row_height(&n);
  assert(gesture(&n,w/2,top-4,w/2,top-4,120000,false)==LITE_NAV_NONE);
  assert(gesture(&n,w/2,top+10,w/2,top+10,120000,false)==LITE_NAV_AOD);
  assert(gesture(&n,w/2,top+row+10,w/2,top+row+10,120000,false)==LITE_NAV_WIFI);
  assert(gesture(&n,w/2,top+row*2+10,w/2,top+row*2+10,120000,false)==LITE_NAV_BRIGHTNESS);
  /* Swipe beginning on a row navigates, never activates that row. */
  assert(gesture(&n,w/2,top+10,w/2,top+row*3,300000,false)==LITE_NAV_CHANGED&&n.page==LITE_CLOCK);
  assert(gesture(&n,w/2,h/4,w/2,h*3/4,300000,false)==LITE_NAV_CHANGED&&n.page==LITE_CONNECTION);
  assert(gesture(&n,w/2,h/2+60,w/2,h/2+60,120000,false)==LITE_NAV_WIFI);
  assert(gesture(&n,w/2,h*3/4,w/2,h/4,300000,false)==LITE_NAV_CHANGED&&n.page==LITE_CLOCK);
  gesture(&n,w/2,h*3/4,w/2,h/4,300000,false);
  assert(gesture(&n,w/2,top+row*4+10,w/2,top+row*4+10,120000,false)==LITE_NAV_CHANGED&&n.page==LITE_CONFIRM_EXIT);
  for(int i=0;i<1000;i++){int x=(i*53)%w,y=(i*97)%h;assert(gesture(&n,x,y,x,y,120000,false)!=LITE_NAV_EXIT);}
  assert(gesture(&n,w/8,h/2,w*7/8,h/2,80000,false)!=LITE_NAV_EXIT);
  assert(gesture(&n,w/2,h/2,w*7/8,h/2,300000,false)!=LITE_NAV_EXIT);
  assert(gesture(&n,w/8,h/2,w/4,h/2,300000,false)!=LITE_NAV_EXIT);
  assert(gesture(&n,w/8,h/2,w*7/8,h/2,300000,false)==LITE_NAV_EXIT);
  assert(lite_navigation_key(&n,true,0,at,false,false,false)==LITE_NAV_SLEEP);
  assert(lite_navigation_key(&n,true,1,at,false,false,false)==LITE_NAV_CHANGED&&n.page==LITE_SETTINGS);
  assert(lite_navigation_key(&n,false,1,at,false,false,false)==LITE_NAV_CHANGED&&n.page==LITE_CONFIRM_EXIT);
  assert(lite_navigation_key(&n,false,3,at+5000000,false,false,false)==LITE_NAV_NONE);
  assert(lite_navigation_key(&n,false,1,at+6000000,false,false,false)==LITE_NAV_EXIT);
  assert(lite_navigation_key(&n,false,0,at,true,false,false)==LITE_NAV_WAKE);
  assert(lite_navigation_key(&n,false,0,at,false,true,false)==LITE_NAV_STOP);
  assert(lite_navigation_key(&n,true,0,at,false,true,false)==LITE_NAV_SNOOZE);
  n.page=LITE_SETTINGS;n.selected=0;lite_navigation_knob(&n,4);assert(n.page==LITE_SETTINGS&&n.selected==4);
  assert(lite_navigation_key(&n,false,0,at,false,false,true)==LITE_NAV_CHANGED&&n.page==LITE_CONFIRM_EXIT);
  assert(lite_navigation_key(&n,false,0,at,false,false,true)==LITE_NAV_CHANGED&&n.page==LITE_SETTINGS);
  n.page=LITE_CLOCK;assert(lite_navigation_key(&n,false,0,at,false,false,false)==LITE_NAV_IDLE);
  assert(gesture(&n,w*3/4,h/2,w/4,h/2,300000,false)==LITE_NAV_CHANGED&&n.page==LITE_APPS);
  /* A transient missing touch sample does not release/activate a row. */
  lite_navigation_touch(&n,true,w/2,top+10,at,false,false);
  assert(lite_navigation_touch(&n,false,0,0,at+100000,false,false)==LITE_NAV_NONE);
  assert(n.down);lite_navigation_touch(&n,true,w/2,top+10,at+110000,false,false);
  assert(n.down);
  printf("PASS %dx%d: gestures, wake, row bounds, 1000 accidental taps, deliberate exit, distinct keys, knob, release debounce\n",w,h);
 }
}
''',encoding='utf-8')
vswhere = Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
vs = Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-property', 'installationPath'], text=True).strip())
raw = subprocess.check_output(f'cmd.exe /d /s /c ""{vs / "Common7/Tools/VsDevCmd.bat"}" -arch=x64 >nul && set"', text=True)
env = dict(os.environ)
for line in raw.splitlines():
    if '=' in line and not line.startswith('='):
        k, v = line.split('=', 1)
        env[k.upper()] = v
compiler = sorted((vs/'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/cl.exe'))[-1]
subprocess.run([str(compiler),'/nologo','/std:c11','/W3','/I'+str(root/'lite/main'),str(work/'test.c'),str(root/'lite/main/navigation.c'),'/Fe:'+str(work/'test.exe')],cwd=work,env=env,check=True)
subprocess.run([str(work/'test.exe')],check=True)

