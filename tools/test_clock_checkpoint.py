"""Compile the actual clock service and test safe software handover invariants."""
from pathlib import Path
import os, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
work = Path(tempfile.mkdtemp(prefix='clock-handover-'))
headers = {
'esp_err.h': '#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\nconst char *esp_err_to_name(int);\n',
'esp_log.h': '#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n',
'esp_timer.h': '#pragma once\n#include <stdint.h>\ntypedef void *esp_timer_handle_t;\ntypedef struct {void(*callback)(void*);const char *name;}esp_timer_create_args_t;\nint esp_timer_create(const esp_timer_create_args_t*,void**);\nint esp_timer_stop(void*);int esp_timer_start_once(void*,uint64_t);\n',
'esp_system.h': '#pragma once\n#define ESP_RST_SW 3\n#define ESP_RST_POWERON 1\nint esp_reset_reason(void);\n',
'esp_rtc_time.h': '#pragma once\n#include <stdint.h>\nuint64_t esp_rtc_get_time_us(void);\n',
'core/state.h': '#pragma once\n#include <stdint.h>\n#define STATE_MINUTE 0\nvoid state_set(int,int32_t);\n',
'nvs.h': '#pragma once\n#include <stddef.h>\ntypedef int nvs_handle_t;\n#define NVS_READONLY 0\n#define NVS_READWRITE 1\nint nvs_open(const char*,int,int*);int nvs_get_blob(int,const char*,void*,size_t*);int nvs_set_blob(int,const char*,const void*,size_t);int nvs_commit(int);void nvs_close(int);\n',
'sys/time.h': '#pragma once\n#include <time.h>\ntypedef long suseconds_t;\nstruct timeval{time_t tv_sec;long tv_usec;};\nint gettimeofday(struct timeval*,void*);int settimeofday(const struct timeval*,const void*);\n'
}
for name, content in headers.items():
    f=work/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(content,encoding='utf-8')
source=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include "sys/time.h"
#include "core/settings.h"
static int64_t utc=1792965600123456LL;static uint64_t counter=50000000;
static unsigned char saved[128];static size_t saved_size;static int reason=3,sets,writes;static bool fail_commit;
static settings_t settings;
time_t fake_time(time_t *out){time_t now=(time_t)(utc/1000000);if(out)*out=now;return now;}
static struct tm *fake_localtime_r(const time_t *t,struct tm *out){return localtime_s(out,t)==0?out:NULL;}
int fake_setenv(const char *a,const char*b,int c){(void)a;(void)b;(void)c;return 0;}
#define time fake_time
#define localtime_r fake_localtime_r
#define setenv fake_setenv
#include "CLOCK_SOURCE"
#undef time
int gettimeofday(struct timeval *tv,void *unused){(void)unused;tv->tv_sec=(time_t)(utc/1000000);tv->tv_usec=(long)(utc%1000000);return 0;}
int settimeofday(const struct timeval *tv,const void *unused){(void)unused;sets++;utc=(int64_t)tv->tv_sec*1000000+tv->tv_usec;return 0;}
const char *esp_err_to_name(int e){(void)e;return "error";}
int esp_reset_reason(void){return reason;}uint64_t esp_rtc_get_time_us(void){return counter;}
int nvs_open(const char *ns,int mode,int *h){assert(!strcmp(ns,"clock_keep"));(void)mode;*h=1;return 0;}
int nvs_set_blob(int h,const char *key,const void *data,size_t n){(void)h;assert(!strcmp(key,"v1")&&n<=sizeof(saved));memcpy(saved,data,n);saved_size=n;writes++;return 0;}
int nvs_get_blob(int h,const char *key,void *data,size_t *n){(void)h;assert(!strcmp(key,"v1"));if(!saved_size||*n<saved_size)return -1;memcpy(data,saved,saved_size);*n=saved_size;return 0;}
int nvs_commit(int h){(void)h;return fail_commit?-1:0;}void nvs_close(int h){(void)h;}
int rtc_chip_get_time(time_t *out){(void)out;return -1;}int rtc_chip_set_time(time_t t){(void)t;return 0;}
const settings_t *settings_get(void){return &settings;}void state_set(int id,int32_t v){(void)id;(void)v;}
int esp_timer_create(const esp_timer_create_args_t *a,void **out){(void)a;*out=(void*)1;return 0;}
int esp_timer_stop(void *h){(void)h;return 0;}int esp_timer_start_once(void*h,uint64_t us){(void)h;assert(us>0&&us<=60020000);return 0;}
int main(void){
    int64_t original=utc;clock_checkpoint();assert(writes==1&&saved_size==sizeof(clock_keep_t));
    counter+=2345678;utc=0;assert(restore_checkpoint());assert(utc==original+2345678);assert(sets==1);
    reason=ESP_RST_POWERON;utc=0;assert(!restore_checkpoint()&&utc==0);
    reason=ESP_RST_SW;counter=1;assert(!restore_checkpoint()&&utc==0);
    counter=50000000+8ULL*86400*1000000;assert(!restore_checkpoint()&&utc==0);
    counter=60000000;saved[0]^=0xff;assert(!restore_checkpoint()&&utc==0);saved[0]^=0xff;
    saved_size--;assert(!restore_checkpoint()&&utc==0);saved_size++;
    int before=writes;utc=0;clock_checkpoint();assert(writes==before);
    puts("CLOCK_HANDOVER_PASS: microsecond continuity on software reset, rejects cold boot, counter rewind, stale checkpoint, corrupt/truncated data, unset clock");
}
'''.replace('CLOCK_SOURCE',(root/'components/core/clock.c').as_posix())
(work/'test.c').write_text(source,encoding='utf-8')
vswhere=Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
vs=Path(subprocess.check_output([str(vswhere),'-latest','-products','*','-property','installationPath'],text=True).strip())
raw=subprocess.check_output(f'cmd.exe /d /s /c ""{vs / "Common7/Tools/VsDevCmd.bat"}" -arch=x64 >nul && set"',text=True)
env=dict(os.environ)
for line in raw.splitlines():
    if '=' in line and not line.startswith('='):
        k,v=line.split('=',1);env[k.upper()]=v
compiler=sorted((vs/'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/cl.exe'))[-1]
subprocess.run([str(compiler),'/nologo','/std:c11','/utf-8','/W3','/D_CRT_SECURE_NO_WARNINGS','/I'+str(work),'/I'+str(root/'components/core/include'),'/I'+str(root/'components/board_api/include'),str(work/'test.c'),'/Fe:'+str(work/'test.exe')],cwd=work,env=env,check=True)
subprocess.run([str(work/'test.exe')],check=True)
