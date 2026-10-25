"""Run the actual Raccolta/Zampe C model with MSVC and the firmware's cJSON."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
cjson = Path(sys.argv[1]).resolve()
work = Path(tempfile.mkdtemp(prefix='life-model-'))
source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "life_model.h"
static int today;
static const cJSON *arr(const cJSON *d,const char *key){return cJSON_GetObjectItemCaseSensitive(d,key);}
/* The runtime commits a copy only after all validation succeeds. */
static bool apply(life_kind_t k,cJSON **doc,const char *json){
    cJSON *c=cJSON_Parse(json),*copy=cJSON_Duplicate(*doc,true);char error[128];
    assert(c&&copy);bool ok=life_apply(k,copy,c,today,error,sizeof(error));cJSON_Delete(c);
    if(ok){cJSON_Delete(*doc);*doc=copy;}else{assert(error[0]);cJSON_Delete(copy);}return ok;
}
int main(void){
    today=life_day("2026-10-25");assert(life_day("1970-01-01")==0);
    assert(life_day("2024-02-29")!=LIFE_NO_DAY);assert(life_day("2100-02-29")==LIFE_NO_DAY);
    assert(life_day("2026-2-01")==LIFE_NO_DAY);assert(life_day("2026-04-31")==LIFE_NO_DAY);
    for(int d=life_day("1900-01-01");d<=life_day("2199-12-31");d++){
        char date[11];life_date(d,date);assert(life_day(date)==d);
        assert(life_weekday(d)>=0&&life_weekday(d)<7);
    }
    assert(life_weekday(life_day("2025-01-13"))==0);
    cJSON *w=life_defaults(LIFE_RACCOLTA),*z=life_defaults(LIFE_ZAMPE);
    assert(cJSON_GetArraySize(arr(z,"pets"))==0);assert(cJSON_GetArraySize(arr(z,"tasks"))==0);
    assert(apply(LIFE_RACCOLTA,&w,"{\"op\":\"city.add\",\"name\":\"Civita Castellana\",\"preset\":\"civita-domestica\"}"));
    const cJSON *city=cJSON_GetArrayItem(arr(w,"cities"),0);int cityid=life_number(city,"id",0);
    assert(life_number(w,"preferred",0)==cityid);
    const int counts[]={1,1,1,2,1,1,0};
    for(int i=0;i<7;i++){cJSON *a=life_collections(city,life_day("2025-01-13")+i);assert(cJSON_GetArraySize(a)==counts[i]);cJSON_Delete(a);}
    char cmd[256];snprintf(cmd,sizeof(cmd),"{\"op\":\"exception.put\",\"city\":%d,\"date\":\"2026-10-26\",\"bins\":[]}",cityid);
    assert(apply(LIFE_RACCOLTA,&w,cmd));city=cJSON_GetArrayItem(arr(w,"cities"),0);
    cJSON *a=life_collections(city,life_day("2026-10-26"));assert(!cJSON_GetArraySize(a));cJSON_Delete(a);
    assert(apply(LIFE_RACCOLTA,&w,"{\"op\":\"city.add\",\"name\":\"Citta di esempio\"}"));
    assert(cJSON_GetArraySize(arr(w,"cities"))==2);
    assert(apply(LIFE_ZAMPE,&z,"{\"op\":\"pet.add\",\"name\":\"Profilo di prova\",\"birth\":\"2024-01-01\",\"sex\":\"female\"}"));
    assert(!apply(LIFE_ZAMPE,&z,"{\"op\":\"pet.add\",\"name\":\"Prova\",\"birth\":\"2090-01-01\"}"));
    assert(cJSON_GetArraySize(arr(z,"pets"))==1);
    int pet=life_number(cJSON_GetArrayItem(arr(z,"pets"),0),"id",0);
    snprintf(cmd,sizeof(cmd),"{\"op\":\"task.add\",\"pet\":%d,\"label\":\"Passeggiata\",\"due\":\"2026-10-20\",\"repeat_days\":2}",pet);
    assert(apply(LIFE_ZAMPE,&z,cmd));int task=life_number(cJSON_GetArrayItem(arr(z,"tasks"),0),"id",0);
    snprintf(cmd,sizeof(cmd),"{\"op\":\"task.edit\",\"id\":%d,\"repeat_days\":1.5}",task);assert(!apply(LIFE_ZAMPE,&z,cmd));
    snprintf(cmd,sizeof(cmd),"{\"op\":\"task.edit\",\"id\":%d,\"notify\":\"false\"}",task);assert(!apply(LIFE_ZAMPE,&z,cmd));
    assert(!cJSON_IsTrue(arr(cJSON_GetArrayItem(arr(z,"tasks"),0),"notify")));
    snprintf(cmd,sizeof(cmd),"{\"op\":\"task.done\",\"id\":%d}",task);assert(apply(LIFE_ZAMPE,&z,cmd));
    assert(!strcmp(life_string(cJSON_GetArrayItem(arr(z,"tasks"),0),"due"),"2026-10-27"));
    assert(!apply(LIFE_ZAMPE,&z,cmd));assert(cJSON_GetArraySize(arr(z,"logs"))==1);
    snprintf(cmd,sizeof(cmd),"{\"op\":\"task.edit\",\"id\":%d,\"notes\":\"Aggiornato\"}",task);assert(apply(LIFE_ZAMPE,&z,cmd));
    assert(!strcmp(life_string(cJSON_GetArrayItem(arr(z,"tasks"),0),"due"),"2026-10-27"));
    snprintf(cmd,sizeof(cmd),"{\"op\":\"weight.add\",\"pet\":%d,\"grams\":2400,\"date\":\"2026-10-25\"}",pet);assert(apply(LIFE_ZAMPE,&z,cmd));
    snprintf(cmd,sizeof(cmd),"{\"op\":\"weight.add\",\"pet\":%d,\"grams\":-1,\"date\":\"2026-10-25\"}",pet);assert(!apply(LIFE_ZAMPE,&z,cmd));
    snprintf(cmd,sizeof(cmd),"{\"op\":\"pet.delete\",\"id\":%d}",pet);assert(apply(LIFE_ZAMPE,&z,cmd));
    assert(!cJSON_GetArraySize(arr(z,"pets"))&&!cJSON_GetArraySize(arr(z,"tasks"))&&!cJSON_GetArraySize(arr(z,"weights"))&&!cJSON_GetArraySize(arr(z,"logs")));
    cJSON_Delete(w);cJSON_Delete(z);
    puts("LIFE_PASS: 109573 date round-trips, weekly preset, exceptions, multiple cities, empty private defaults, transactional validation, recurrence, completion, weights, cascading deletion");
}
'''
(work/'test.c').write_text(source, encoding='utf-8')
vswhere = Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
vs = Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-property', 'installationPath'], text=True).strip())
raw = subprocess.check_output(f'cmd.exe /d /s /c ""{vs / "Common7/Tools/VsDevCmd.bat"}" -arch=x64 >nul && set"', text=True)
env = dict(os.environ)
for line in raw.splitlines():
    if '=' in line and not line.startswith('='):
        k, v = line.split('=', 1)
        env[k.upper()] = v
compiler = sorted((vs/'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/cl.exe'))[-1]
subprocess.run([str(compiler), '/nologo', '/std:c11', '/utf-8', '/W3', '/D_CRT_SECURE_NO_WARNINGS',
    '/I'+str(cjson), '/I'+str(root/'components/apps/life'), str(work/'test.c'),
    str(root/'components/apps/life/life_model.c'), str(cjson/'cJSON.c'), '/Fe:'+str(work/'test.exe')], cwd=work, env=env, check=True)
subprocess.run([str(work/'test.exe')], check=True)
