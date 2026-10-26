"""Compile the actual bounded device parser with MSVC and cJSON."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[2]
cjson=Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='agent-deck-model-') as tmp:
    work=Path(tmp)
    (work/'test.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "model.h"
static cJSON *parse(const char *s){cJSON *i=cJSON_Parse(s);assert(i);cJSON *o=agentdeck_validate(i);cJSON_Delete(i);return o;}
int main(void){
 cJSON *o=parse("{\"schema\":1,\"generated_at\":1,\"cards\":[]}");assert(o);cJSON_Delete(o);
 assert(!parse("{\"schema\":2,\"generated_at\":1,\"cards\":[]}"));
 assert(!parse("{\"schema\":1,\"generated_at\":1,\"cards\":[{\"id\":\"x\"}]}"));
 const char *valid="{\"schema\":1,\"generated_at\":1,\"cards\":[{\"id\":\"x\",\"agent\":\"codex\",\"name\":\"Build\",\"status\":\"needs_input\",\"context_percent\":45,\"secret\":\"discard\",\"quota\":[{\"name\":\"primary\",\"remaining_percent\":70}]}]}";
 o=parse(valid);assert(o);cJSON *cards=cJSON_GetObjectItemCaseSensitive(o,"cards"),*card=cJSON_GetArrayItem(cards,0);assert(!cJSON_GetObjectItemCaseSensitive(card,"secret"));
 cJSON *copy=cJSON_Duplicate(card,1);assert(copy);cJSON_AddItemToArray(cards,copy);assert(!agentdeck_validate(o));cJSON_Delete(o);
 o=cJSON_Parse(valid);card=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(o,"cards"),0);cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(card,"context_percent"),101);assert(!agentdeck_validate(o));cJSON_Delete(o);
 o=cJSON_Parse(valid);card=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(o,"cards"),0);cJSON *q=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(card,"quota"),0);cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(q,"remaining_percent"),-1);assert(!agentdeck_validate(o));cJSON_Delete(o);
 o=cJSON_Parse(valid);card=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(o,"cards"),0);char name[102];memset(name,'a',101);name[101]=0;cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(card,"name"),name);assert(!agentdeck_validate(o));cJSON_Delete(o);
 o=cJSON_Parse(valid);cards=cJSON_GetObjectItemCaseSensitive(o,"cards");for(int i=0;i<16;i++)cJSON_AddItemToArray(cards,cJSON_CreateObject());assert(!agentdeck_validate(o));cJSON_Delete(o);
 puts("DEVICE MODEL PASS: bounds, canonical fields, duplicate IDs, context and quota limits");
}
''',encoding='utf-8')
    vswhere=Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
    vs=Path(subprocess.check_output([str(vswhere),'-latest','-products','*','-property','installationPath'],text=True).strip())
    raw=subprocess.check_output(f'cmd.exe /d /s /c ""{vs / "Common7/Tools/VsDevCmd.bat"}" -arch=x64 >nul && set"',text=True)
    env=dict(os.environ)
    for line in raw.splitlines():
        if '=' in line and not line.startswith('='):
            k,v=line.split('=',1);env[k.upper()]=v
    cl=sorted((vs/'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/cl.exe'))[-1]
    subprocess.run([str(cl),'/nologo','/std:c11','/W3','/D_CRT_SECURE_NO_WARNINGS','/I'+str(cjson),'/I'+str(root/'components/apps/agentdeck'),str(work/'test.c'),str(root/'components/apps/agentdeck/model.c'),str(cjson/'cJSON.c'),'/Fe:'+str(work/'test.exe')],cwd=work,env=env,check=True)
    subprocess.run([str(work/'test.exe')],check=True)
