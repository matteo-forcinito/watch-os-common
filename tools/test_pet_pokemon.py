"""Actual C renderer + NVS sidecar tests with the generated personal sprite pack."""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
assets = Path(sys.argv[1]).resolve()
work = Path(tempfile.mkdtemp(prefix='pet-pokemon-'))
headers = {
    'freertos/FreeRTOS.h': '#define portMAX_DELAY 0\n',
    'freertos/semphr.h': 'typedef int StaticSemaphore_t; typedef void *SemaphoreHandle_t;\n#define xSemaphoreCreateMutexStatic(p) ((void*)(p))\n#define xSemaphoreTake(a,b) ((void)0)\n#define xSemaphoreGive(a) ((void)0)\n',
    'nvs.h': '#include <stddef.h>\ntypedef int nvs_handle_t;\n#define ESP_OK 0\n#define NVS_READONLY 0\n#define NVS_READWRITE 1\nint nvs_open(const char*,int,nvs_handle_t*);\nint nvs_get_blob(int,const char*,void*,size_t*);\nint nvs_set_blob(int,const char*,const void*,size_t);\nint nvs_commit(int);\nvoid nvs_close(int);\n',
}
for name, value in headers.items():
    path = work/name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value)
source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "orbi/pet_sprite.h"
#include "PET_SOURCE"
static unsigned char durable[1024], pending[1024];
static size_t stored, pending_size;
static bool fail;
int nvs_open(const char *ns,int mode,int *h){assert(!strcmp(ns,"pet_looks"));(void)mode;*h=1;return 0;}
int nvs_get_blob(int h,const char*k,void*out,size_t*n){(void)h;assert(!strcmp(k,"v1"));if(!stored)return 1;assert(*n>=stored);memcpy(out,durable,stored);*n=stored;return 0;}
int nvs_set_blob(int h,const char*k,const void*p,size_t n){(void)h;assert(!strcmp(k,"v1"));assert(n<sizeof(pending));memcpy(pending,p,n);pending_size=n;return 0;}
int nvs_commit(int h){(void)h;if(fail)return 1;memcpy(durable,pending,pending_size);stored=pending_size;return 0;}
void nvs_close(int h){(void)h;}
int main(void){
    uint16_t dex;bool shiny;uint8_t pixels[1024];uint32_t palette[PET_COLORS];
    pet_look_t original,look;uint64_t seed=0x123456789abcdefULL;
    pet_look(seed,7,&original);assert(!original.pokemon_dex);
    pet_pokemon_init();assert(pet_pokemon_count()==31);
    assert(pet_pokemon_set(seed,25,true));pet_look(seed,7,&look);
    assert(look.pokemon_dex==25&&look.pokemon_shiny);
    assert(look.species==original.species&&look.variant==original.variant&&look.level==original.level);
    fail=true;assert(!pet_pokemon_set(seed,4,false));fail=false;
    pet_pokemon_get(seed,&dex,&shiny);assert(dex==25&&shiny);
    s_ready=false;memset(&s_store,0,sizeof(s_store));pet_pokemon_init();
    pet_pokemon_get(seed,&dex,&shiny);assert(dex==25&&shiny);
    assert(!pet_pokemon_set(seed,151,false));assert(!pet_pokemon_set(0,25,false));
    for(int i=0;i<pet_pokemon_count();i++) for(int rare=0;rare<2;rare++){
        dex=pet_pokemon_dex(i);assert(pet_pokemon_set(seed,dex,rare!=0));pet_look(seed,7,&look);
        pet_palette(&look,0,palette);assert(palette[0]==0);
        const pokemon_sprite_t *s=sprite(dex,rare!=0);assert(s&&s->frames);
        for(unsigned f=0;f<8;f++){
            pet_draw(&look,PET_POSE_IDLE,f,false,pixels);bool visible=false;
            for(unsigned y=0;y<32;y++)for(unsigned x=0;x<32;x++){
                unsigned raw=s->pixels[(f%s->frames)*1600+(y*40/32)*40+x*40/32];
                unsigned idx=pixels[y*32+x];assert(idx==(raw==255?0:raw+1));
                if(idx){assert(palette[idx]>>24==255);visible=true;}
            }assert(visible);
        }
    }
    assert(pet_pokemon_set(seed,0,false));pet_look(seed,7,&look);
    assert(!memcmp(&look,&original,sizeof(look)));
    for(uint64_t i=1;i<=8;i++)assert(pet_pokemon_set(i,25,false));
    assert(!pet_pokemon_set(9,25,false));assert(pet_pokemon_set(4,0,false));assert(pet_pokemon_set(9,4,false));
    puts("PET_POKEMON_PASS: 62 variants, 496 frames, persistence, failed commit, identity, fallback, capacity");
    return 0;
}
'''.replace('PET_SOURCE', (root/'components/orbi/pet_pokemon.c').as_posix())
(work/'test.c').write_text(source)
vswhere = Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
vs = Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-property', 'installationPath'], text=True).strip())
raw = subprocess.check_output(f'cmd.exe /d /s /c ""{vs / "Common7/Tools/VsDevCmd.bat"}" -arch=x64 >nul && set"', text=True)
env = dict(os.environ)
for line in raw.splitlines():
    if '=' in line and not line.startswith('='):
        k, v = line.split('=', 1);env[k.upper()] = v
compiler = sorted((vs/'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/cl.exe'))[-1]
subprocess.run([str(compiler), '/nologo', '/std:c11', '/W4', '/WX', '/DWATCH_PET_POKEMON=1',
    '/I'+str(work), '/I'+str(root/'components/orbi/include'), '/I'+str(root/'components/orbi/pokemon'),
    str(work/'test.c'), str(root/'components/orbi/pet_sprite.c'), str(assets/'pokemon_assets.c'),
    '/Fe:'+str(work/'test.exe')], cwd=work, env=env, check=True)
subprocess.run([str(work/'test.exe')], check=True)
