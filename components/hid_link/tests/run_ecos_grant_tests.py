"""Execute production ECOS grant/NVS/key derivation entry points with SDK faults."""
from pathlib import Path
import subprocess
import tempfile

tests = Path(__file__).resolve().parent
source = (tests.parent / 'hid_hub.c').read_text(encoding='utf-8')
start = source.index('static bool family_store(')
end = source.index('static bool confirm_tag(', start)
production = source[start:end]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND -2
#define ESP_ERR_NO_MEM -3
#define ESP_ERR_INVALID_STATE -4
#define ESP_MAC_WIFI_STA 0
#define NVS_READONLY 0
#define PSA_SUCCESS 0
#define HID_HUB_NAME 24
#define HID_HUB_PEERS 4
#define HID_HUB_OFF 0
#define HID_HUB_HUB 1
#define HID_HUB_SATELLITE 2
#define portMAX_DELAY 0xffffffffu
#define atomic_load(p) (*(p))
typedef struct { char name[24]; uint8_t mac[6]; int rssi; bool connected; } hid_hub_peer_info_t;
typedef struct { uint8_t used, mac[6], key[16]; char name[24]; } peer_t;
typedef struct { uint8_t schema, role, reserved[2]; peer_t peers[4]; } config_t;
static config_t s_config, saved;
static bool s_started=true;
static void *s_lock;
static uint8_t disk[HID_ECOS_STORE], self[6]={2,1,2,3,4,5}, peer[6]={4,1,2,3,4,5};
static int fail_open, fail_read, fail_crypto, fail_mac, fail_hmac, fail_save;
static int saves, opens, closes, locks;
static size_t disk_length=HID_ECOS_STORE;
static void config_load(void) {}
static int xSemaphoreTake(void *p,unsigned n) {(void)p;(void)n;locks++;return 1;}
static void xSemaphoreGive(void *p) {(void)p;locks--;}
static int psa_crypto_init(void) {return fail_crypto;}
static esp_err_t esp_read_mac(uint8_t out[6],int type) {(void)type;memcpy(out,self,6);return fail_mac;}
static esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *handle) {
 assert(!strcmp(ns,"ecos")&&mode==NVS_READONLY);if(fail_open)return fail_open;opens++;*handle=1;return 0;
}
static esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *length) {
 assert(h==1&&!strcmp(key,"peers")&&*length==HID_ECOS_STORE);memcpy(out,disk,HID_ECOS_STORE);*length=disk_length;return fail_read;
}
static void nvs_close(nvs_handle_t handle) {assert(handle==1);closes++;}
static bool hmac(const uint8_t *key,size_t kl,const uint8_t *context,size_t cl,uint8_t out[32]) {
 assert(kl==32&&cl==24&&!memcmp(context,"RHUB-ECOS-v1",12));
 assert(!memcmp(context+12,self,6)&&!memcmp(context+18,peer,6));
 for(unsigned i=0;i<32;i++)out[i]=key[i]^context[i%24];return !fail_hmac;
}
static esp_err_t config_save(const config_t *config) {saves++;saved=*config;return fail_save;}
'''
suffix = r'''
static void reset(void) {
 memset(disk,0,sizeof(disk));disk[0]=1;disk[1]=1;memcpy(disk+2,peer,6);
 memcpy(disk+8,"Member",7);memset(disk+32,0x43,32);
 memset(&s_config,0,sizeof(s_config));s_config.schema=2;s_config.role=HID_HUB_HUB;
 fail_open=fail_read=fail_crypto=fail_mac=fail_hmac=fail_save=saves=opens=closes=locks=0;
 disk_length=HID_ECOS_STORE;s_lock=(void*)1;
}
int main(void) {
 reset();hid_hub_peer_info_t members[6];assert(hid_hub_family_peers(members,6)==1);
 assert(!memcmp(members[0].mac,peer,6)&&!strcmp(members[0].name,"Member"));assert(opens==closes);
 assert(hid_hub_family_authorize(peer)==ESP_OK&&saves==1&&locks==0&&opens==closes);
 assert(saved.peers[0].used==2&&!memcmp(saved.peers[0].mac,peer,6));
 assert(s_config.peers[0].used==0); /* Restart applies the saved grant. */
 uint8_t expected[16];memcpy(expected,saved.peers[0].key,16);disk[32]^=1;
 uint8_t rotated[16];assert(family_key(peer,rotated)&&memcmp(expected,rotated,16));
 int *faults[]={&fail_open,&fail_read,&fail_crypto,&fail_mac,&fail_hmac,&fail_save};
 for(unsigned i=0;i<6;i++){reset();*faults[i]=-99;assert(hid_hub_family_authorize(peer)!=ESP_OK);assert(opens==closes&&locks==0);if(i<5)assert(saves==0);}
 reset();disk[0]=2;assert(hid_hub_family_authorize(peer)==ESP_ERR_NOT_FOUND&&saves==0);
 reset();disk_length--;assert(hid_hub_family_authorize(peer)==ESP_ERR_NOT_FOUND&&saves==0);
 reset();s_config.role=HID_HUB_OFF;assert(hid_hub_family_authorize(peer)==ESP_ERR_INVALID_STATE&&saves==0);
 reset();for(unsigned i=0;i<4;i++){s_config.peers[i].used=1;s_config.peers[i].mac[0]=6;}
 assert(hid_hub_family_authorize(peer)==ESP_ERR_NO_MEM&&saves==0);
 s_config.role=HID_HUB_SATELLITE;assert(hid_hub_family_authorize(peer)==ESP_OK&&saved.peers[0].used==2);
 reset();disk[1]=0;assert(hid_hub_family_peers(members,6)==0);
 assert(hid_hub_family_authorize(peer)==ESP_ERR_NOT_FOUND&&saves==0);
 puts("PASS: production ECOS Remote grants, NVS faults, key binding and explicit restart");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='remote-ecos-grants-') as directory:
    directory = Path(directory)
    cfile = directory / 'test.c'
    cfile.write_text('#include "' + (tests.parent / 'hid_hub_ecos.h').as_posix() + '"\n' + prefix + production + suffix)
    exe = directory / 'test.exe'
    batch = directory / 'run.cmd'
    vcvars = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'
    batch.write_text(f'@echo off\ncall "{vcvars}" >nul && cl /nologo /std:c11 /W3 "{cfile}" /Fo:"{directory / "test.obj"}" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"\n')
    subprocess.run(['cmd.exe','/d','/c',str(batch)],check=True)
