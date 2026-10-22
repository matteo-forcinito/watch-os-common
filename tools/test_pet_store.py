"""Run the actual C pet store on Windows, with in-memory NVS and board stubs.
Requires MSVC and an existing firmware managed_components/espressif__cjson.
No device, filesystem NVS, network or credentials are used by the tests.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
workspace = root.parent
stub = Path(tempfile.mkdtemp(prefix='orbi-host-'))
json_dir = workspace/'.release-work/ESP32-S3-Touch-AMOLED-1.75C/managed_components/espressif__cjson/cJSON'
headers = {
    'lvgl.h': 'typedef struct lv_obj_t lv_obj_t;\n',
    'esp_err.h': '#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NVS_NOT_FOUND 4354\n',
    'esp_mac.h': '#include "esp_err.h"\n#include <stdint.h>\n#define ESP_MAC_WIFI_STA 0\nesp_err_t esp_read_mac(uint8_t*,int);\n',
    'esp_random.h': '#include <stdint.h>\nuint32_t esp_random(void);\n',
    'nvs.h': '#include "esp_err.h"\n#include <stddef.h>\ntypedef int nvs_handle_t;\n#define NVS_READWRITE 1\nint nvs_open(const char*,int,nvs_handle_t*);\nint nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);\nint nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);\nint nvs_commit(nvs_handle_t);\nvoid nvs_close(nvs_handle_t);\n',
    'hardware/board.h': '#include <stdbool.h>\ntypedef struct {const char *name; bool round,has_audio,has_knob,has_camera;} board_info_t;\nconst board_info_t *board_info(void);\n',
    'hardware/microphone.h': '#include <stdbool.h>\nbool microphone_supported(void);\n',
    'hardware/imu.h': '#include <stdbool.h>\nbool imu_supported(void);\n',
    'core/sys.h': 'const char *sys_nickname(void);\n',
    'freertos/FreeRTOS.h': '#define portMAX_DELAY 0\n',
    'freertos/semphr.h': 'typedef void *SemaphoreHandle_t;\n#define xSemaphoreCreateMutex() ((void*)1)\n#define xSemaphoreTake(a,b) ((void)0)\n#define xSemaphoreGive(a) ((void)0)\n',
}
for name, text in headers.items():
    path=stub/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
source = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PET_SOURCE"
static unsigned char durable[2][8192], pending_blob[8192];
static size_t lengths[2], pending_len;
static int device, fail_commit;
static unsigned serial=200;
int nvs_open(const char*a,int b,int*c){(void)a;(void)b;*c=device;return 0;}
int nvs_get_blob(int h,const char*k,void*p,size_t*n){(void)k;if(!lengths[h])return ESP_ERR_NVS_NOT_FOUND;assert(*n>=lengths[h]);*n=lengths[h];memcpy(p,durable[h],*n);return 0;}
int nvs_set_blob(int h,const char*k,const void*p,size_t n){(void)h;(void)k;assert(n<8192);memcpy(pending_blob,p,n);pending_len=n;return 0;}
int nvs_commit(int h){if(fail_commit)return 9;memcpy(durable[h],pending_blob,pending_len);lengths[h]=pending_len;return 0;}
void nvs_close(int h){(void)h;}
int esp_read_mac(uint8_t *m,int t){(void)t;memset(m,0,6);m[5]=(uint8_t)(device+1);return 0;}
uint32_t esp_random(void){return ++serial;}
const board_info_t *board_info(void){static board_info_t b={"Test device",true,true,false};return &b;}
bool microphone_supported(void){return true;}
bool imu_supported(void){return true;}
const char *sys_nickname(void){return device?"Desk":"Wrist";}
static void boot(int d){device=d;s_ready=false;memset(&s_store,0,sizeof(s_store));assert(pet_init());}
static cJSON *cmd(const char *action,const char *seed,const char *tx,cJSON *pet){
 cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"action",action);cJSON_AddStringToObject(j,"seed",seed);
 if(tx)cJSON_AddStringToObject(j,"transfer",tx);if(pet)cJSON_AddItemToObject(j,"pet",cJSON_Duplicate(pet,1));
 const char *err; cJSON *out=pet_command(j,&err);cJSON_Delete(j);return out;
}
static void good(const char*a,const char*s,const char*t,cJSON*p){cJSON *r=cmd(a,s,t,p);assert(r);cJSON_Delete(r);}
int main(void){
 const char *tx="0123456789abcdef0123456789abcdef";
 boot(0);pet_record_t a;assert(pet_current(&a));char seed[17];strcpy(seed,a.seed);
 good("pet",seed,NULL,NULL);assert(pet_current(&a));assert(a.xp==3);
 boot(0);assert(pet_current(&a));assert(!strcmp(a.seed,seed)&&a.xp==3);
 fail_commit=1;assert(!cmd("feed",seed,NULL,NULL));assert(pet_current(&a));assert(a.xp==3);fail_commit=0;
 good("reserve",seed,tx,NULL);good("reserve",seed,tx,NULL);assert(!pet_current(&a));assert(!cmd("pet",seed,NULL,NULL));
 cJSON *all=pet_snapshot(),*arr=cJSON_GetObjectItem(all,"pets"),*snapshot=cJSON_Duplicate(cJSON_GetArrayItem(arr,0),1);cJSON_Delete(all);
 boot(1);pet_record_t native;assert(pet_current(&native));assert(strcmp(native.seed,seed));
 cJSON *bad=cJSON_Duplicate(snapshot,1);cJSON_ReplaceItemInObject(bad,"xp",cJSON_CreateNumber(-1));assert(!cmd("receive",seed,tx,bad));cJSON_Delete(bad);
 good("receive",seed,tx,snapshot);good("receive",seed,tx,snapshot);boot(1);assert(pet_current(&native));assert(strcmp(native.seed,seed));
 boot(0);good("depart",seed,tx,NULL);good("depart",seed,tx,NULL);boot(0);assert(!pet_current(&a));
 assert(!cmd("cancel",seed,tx,NULL));assert(!cmd("arrive",seed,"ffffffffffffffff",NULL));
 boot(1);good("arrive",seed,tx,NULL);good("arrive",seed,tx,NULL);assert(pet_current(&a));assert(!strcmp(a.seed,seed)&&a.xp==3);assert(!strcmp(a.origin,"Wrist"));
 boot(0);good("forget",seed,tx,NULL);good("forget",seed,tx,NULL);boot(0);assert(!pet_current(&a));
 boot(1);assert(pet_current(&a));assert(a.xp==3);good("sleep",seed,NULL,NULL);assert(!cmd("play",seed,NULL,NULL));good("sleep",seed,NULL,NULL);
 const char *tx2="abcdef0123456789abcdef0123456789";
 good("reserve",seed,tx2,NULL);all=pet_snapshot();arr=cJSON_GetObjectItem(all,"pets");cJSON *item=NULL;
 for(int i=0;i<cJSON_GetArraySize(arr);i++){cJSON *v=cJSON_GetArrayItem(arr,i);if(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(v,"seed")),seed))item=v;}
 cJSON *back=cJSON_Duplicate(item,1);cJSON_Delete(all);
 boot(0);good("receive",seed,tx2,back);good("cancel",seed,tx2,NULL);good("cancel",seed,tx2,NULL);
 boot(1);good("cancel",seed,tx2,NULL);assert(pet_current(&a));assert(!strcmp(a.seed,seed));
 cJSON_Delete(snapshot);cJSON_Delete(back);puts("C pet store: birth, reboot, care, NVS rollback, validation, transfers, retry and cancellation OK");
 return 0;
}
'''.replace('PET_SOURCE',(root/'components/apps/pet/pet_store.c').as_posix())
(stub/'test.c').write_text(source)
vswhere=Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
vs=Path(subprocess.check_output([str(vswhere),'-latest','-products','*','-property','installationPath'],text=True).strip())
setup=vs/'Common7/Tools/VsDevCmd.bat'
raw=subprocess.check_output(f'cmd.exe /d /s /c ""{setup}" -arch=x64 >nul && set"',text=True)
env=dict(os.environ)
for line in raw.splitlines():
    if '=' in line and not line.startswith('='):
        k,v=line.split('=',1);env[k.upper()]=v
compiler=sorted((vs/'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/cl.exe'))[-1]
subprocess.run([str(compiler),'/nologo','/std:c11','/W3','/D_CRT_SECURE_NO_WARNINGS',
                '/I'+str(stub),'/I'+str(json_dir),'/I'+str(root/'components/orbi/include'),
                str(stub/'test.c'),str(root/'components/orbi/orbi_genome.c'),str(json_dir/'cJSON.c'),
                '/Fe:'+str(stub/'pet_tests.exe')],env=env,cwd=stub,check=True)
subprocess.run([str(stub/'pet_tests.exe')],check=True)
