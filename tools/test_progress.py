"""Compile the real progress backend against a fault-injecting host NVS."""
from pathlib import Path
import subprocess

def run(root, work, compiler, env, sources):
    headers = {
        'esp_heap_caps.h': '#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n#define heap_caps_calloc(n,s,c) calloc(n,s)\n#define heap_caps_malloc(n,c) malloc(n)\n',
        'esp_log.h': '#include <stdarg.h>\nstatic void log_stub(const char*t,const char*f,...){(void)t;(void)f;}\n#define ESP_LOGI log_stub\n#define ESP_LOGW log_stub\n#define ESP_LOGE log_stub\n',
        'nvs.h': '#include <stddef.h>\ntypedef int esp_err_t;\ntypedef int nvs_handle_t;\n#define ESP_OK 0\n#define ESP_FAIL 3\n#define ESP_ERR_NO_MEM 2\n#define NVS_READONLY 0\n#define NVS_READWRITE 1\nstatic const char*esp_err_to_name(int x){(void)x;return "error";}\nint nvs_open(const char*,int,int*);\nint nvs_get_blob(int,const char*,void*,size_t*);\nint nvs_set_blob(int,const char*,const void*,size_t);\nint nvs_commit(int);\nvoid nvs_close(int);\nint nvs_erase_key(int,const char*);\n',
        'freertos/FreeRTOS.h': '#define portMAX_DELAY 0\n',
        'freertos/semphr.h': 'typedef void*SemaphoreHandle_t;\n#define xSemaphoreCreateMutex() ((void*)1)\n#define xSemaphoreTake(a,b) ((void)(a))\n#define xSemaphoreGive(a) ((void)(a))\n#define vSemaphoreDelete(a) ((void)(a))\n',
        'services/storage.h': '#include <stdbool.h>\nconst char*storage_dir(void);\nbool storage_files_acquire(void);\nvoid storage_files_release(void);\n',
        'unistd.h': '#include <io.h>\n#include <direct.h>\n#define fsync _commit\n#define fileno _fileno\n#define mkdir(p,m) _mkdir(p)\n',
        'core/sys.h': '#include <stdbool.h>\nbool sys_post(void(*fn)(void*),void*);\n',
    }
    stub = work/'progress_stubs'
    for name, text in headers.items():
        p = stub/name; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
    used = [p for p in sources if p.name != 'test_minigames.c']
    subprocess.run([str(compiler), '/nologo', '/std:c11', '/utf-8', '/O2', '/W4', '/WX',
                    '/I'+str(stub), '/I'+str(root/'components/apps/minigames'), '/I'+str(root/'components/apps/pet'),
                    str(root/'tools/test_progress.c'), *map(str, used), '/Fe:'+str(work/'progress.exe')],
                   cwd=work, env=env, check=True)
    subprocess.run([str(work/'progress.exe')], check=True)
