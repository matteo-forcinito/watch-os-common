"""Compile the firmware parser with MSVC and verify a real shared catalogue.

Usage: python test_market_catalog.py PATH_TO_CJSON CATALOG_JSON
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
cjson, catalog = (Path(p).resolve() for p in sys.argv[1:3])
work = Path(tempfile.mkdtemp(prefix='market-catalog-'))
(work/'esp_err.h').write_text('typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 1\n#define ESP_ERR_INVALID_RESPONSE 2\n#define ESP_ERR_NO_MEM 3\n')
(work/'sdkconfig.h').write_text('#define CONFIG_WATCH_DEV_CONSOLE 1\n')
(work/'test.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "services/market.h"
#include "cJSON.h"
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
 char *text=calloc(n+1,1);assert(text);assert(fread(text,1,n,f)==n);fclose(f);
 cJSON *root=cJSON_Parse(text),*apps=cJSON_GetObjectItemCaseSensitive(root,"apps");assert(cJSON_IsArray(apps));
 market_entry_t *entries=calloc(MARKET_MAX_APPS,sizeof(*entries));assert(entries);int count;
 const char *boards[]={"amoled_watch","amoled175_watch","amoled175c_watch","knob_os","watcher_os","guition4848_panel"};
 for(int i=0;i<6;i++){
  int expected=0;cJSON *app;cJSON_ArrayForEach(app,apps){const char *b=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(app,"board"));if(b&&!strcmp(b,boards[i]))expected++;}
  assert(expected>0);assert(market_parse_catalog(text,boards[i],entries,MARKET_MAX_APPS,&count)==ESP_OK);assert(count==expected);
  printf("REAL CATALOG PASS %s: %d apps\n",boards[i],count);
 }
 assert(market_parse_catalog(text,"watcher_os",entries,1,&count)!=ESP_OK&&count==0);
 assert(market_catalog_selftest());
 cJSON_Delete(root);free(text);
 root=cJSON_CreateObject();cJSON_AddNumberToObject(root,"schema",2);apps=cJSON_AddArrayToObject(root,"apps");
 for(int i=0;i<MARKET_CATALOG_ENTRIES_MAX;i++){cJSON *a=cJSON_CreateObject();cJSON_AddStringToObject(a,"board","another_board");cJSON_AddItemToArray(apps,a);}
 text=cJSON_PrintUnformatted(root);assert(market_parse_catalog(text,boards[0],entries,MARKET_MAX_APPS,&count)==ESP_OK&&count==0);free(text);
 cJSON *a=cJSON_CreateObject();cJSON_AddStringToObject(a,"board","another_board");cJSON_AddItemToArray(apps,a);
 text=cJSON_PrintUnformatted(root);assert(market_parse_catalog(text,boards[0],entries,MARKET_MAX_APPS,&count)!=ESP_OK&&count==0);
 free(text);cJSON_Delete(root);free(entries);puts("BOUNDARIES, CAPACITY AND SECURITY SELFTEST PASS");
}
''', encoding='utf-8')
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
    '/I'+str(work), '/I'+str(cjson), '/I'+str(root/'components/services/include'),
    str(work/'test.c'), str(root/'components/services/apps/market_catalog.c'),
    str(root/'components/services/update/fw_version.c'), str(cjson/'cJSON.c'), '/Fe:'+str(work/'test.exe')],
    cwd=work, env=env, check=True)
subprocess.run([str(work/'test.exe'), str(catalog)], check=True)
