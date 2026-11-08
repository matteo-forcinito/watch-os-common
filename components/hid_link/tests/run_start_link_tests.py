"""Extract and execute the production hid_hub_start with MSVC fault stubs."""
from pathlib import Path
import subprocess
import tempfile
import re

test_dir = Path(__file__).resolve().parent
source = test_dir.parent / "hid_hub.c"
text = source.read_text(encoding="utf-8")
marker = "esp_err_t hid_hub_start(const char *name)"
begin = text.index(marker)
brace = text.index("{", begin)
depth = 0
end = None
for i in range(brace, len(text)):
    if text[i] == "{": depth += 1
    elif text[i] == "}":
        depth -= 1
        if depth == 0:
            end = i + 1
            break
assert end is not None
production_start = text[begin:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef bool atomic_bool;
#define atomic_load(p) (*(p))
#define atomic_store(p,v) (*(p)=(v))
#define atomic_compare_exchange_strong(p,e,v) ((*(p)==*(e))?(*(p)=(v),true):(*(e)=*(p),false))
typedef int esp_err_t; typedef uint32_t psa_key_id_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define PSA_SUCCESS 0
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_NOT_SUPPORTED -4
#define ESP_ERR_INVALID_ARG -5
#define ESP_ERR_NOT_FOUND -6
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define ESP_PM_NO_LIGHT_SLEEP 0
#define HID_HUB_NAME 24
#define HID_HUB_PEERS 4
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define HID_HUB_OFF 0
#define HID_HUB_HUB 1
#define HID_HUB_SATELLITE 2
#define CHANNEL 1
#define HID_BUS_TAG "RHUB"
#define ESP_MAC_WIFI_STA 0
#define HUB_PACKET_SIZE 24
#define SYS_CORE_ANY 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef void *SemaphoreHandle_t; typedef void *QueueHandle_t; typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef struct { uint8_t used, mac[6], key[16]; char name[HID_HUB_NAME]; } peer_t;
typedef struct { uint8_t schema, role, reserved[2]; peer_t peers[HID_HUB_PEERS]; } config_t;
typedef struct { int opaque; } hub_state_t;
typedef struct { uint8_t mac[6]; int8_t rssi; uint8_t length, data[120]; } rx_t;
typedef struct { unsigned node; uint8_t packet[HUB_PACKET_SIZE]; } local_t;
typedef struct { int opaque; } esp_now_recv_info_t;
typedef void (*ecos_bus_recv_t)(const esp_now_recv_info_t *,const uint8_t *,int);
typedef struct { int opaque; } esp_pm_lock_handle_t;
static config_t s_config; static char s_name[HID_HUB_NAME]; static uint8_t s_mac[6];
static psa_key_id_t s_keys[HID_HUB_PEERS]; static hub_state_t s_state; static SemaphoreHandle_t s_lock,s_sender,s_start_gate;
static QueueHandle_t s_rx,s_local,s_ack; static TaskHandle_t s_worker_task,s_heartbeat_task;
static esp_pm_lock_handle_t *s_power; static atomic_bool s_started; static uint64_t s_boot_nonce; static uint32_t s_sender_epoch;
static const uint8_t s_bus_pattern[4]={'R',0x40,0,0}, s_bus_mask[4]={0xff,0xe7,0,0};
static const uint8_t s_broadcast[6]={0xff,0xff,0xff,0xff,0xff,0xff};
static bool s_bus_open,s_starting;
static int fail_alloc_at, alloc_calls, fail_stage, stage_count, live_alloc, radio_live,bus_live,pm_live,pm_acquired,task_live,task_deletes,gate_gives;
static int next_handle=1, imported_keys;
static bool family_present;
static void *handle(void) { return (void *)(uintptr_t)next_handle++; }
static void config_load(void) { if(!s_config.schema){s_config.schema=2;s_config.role=HID_HUB_HUB;} }
static int psa_crypto_init(void) { return ESP_OK; }
static bool wifi_is_connected(void) { return false; }
static uint8_t wifi_radio_channel(void) { return 0; }
static int channel_live;
static uint8_t wifi_radio_pinned_channel(void) { return 0; }
static uint8_t wifi_radio_common_channel(void) { return 6; }
static bool wifi_radio_channel_reserve(const char *owner,uint8_t c) { (void)owner;assert(c==6);stage_count++;if(fail_stage==stage_count)return false;channel_live++;return true; }
static void wifi_radio_channel_release(const char *owner) { (void)owner;assert(channel_live==1);channel_live--; }
static esp_err_t ecos_bus_open_mask(const char *tag,const uint8_t *p,const uint8_t *m,ecos_bus_recv_t recv,void *sent,int power) { (void)tag;(void)p;(void)m;(void)recv;(void)sent;(void)power;stage_count++;if(fail_stage==stage_count)return ESP_FAIL;bus_live++;radio_live++;return ESP_OK; }
static void ecos_bus_close(const char *tag) {(void)tag;if(bus_live)bus_live--;if(radio_live)radio_live--;}
static esp_err_t esp_read_mac(uint8_t *m,int t) {(void)t;memset(m,1,6);stage_count++;return fail_stage==stage_count?ESP_FAIL:ESP_OK;}
static psa_key_id_t import_aes(const uint8_t *k) {(void)k;imported_keys++;stage_count++;return fail_stage==stage_count?0:7;}
static bool family_key(const uint8_t *mac, uint8_t key[16]) { (void)mac; memset(key, 3, 16); return family_present; }
static esp_err_t ecos_bus_peer(const uint8_t *mac) {(void)mac;return ESP_OK;}
static esp_err_t esp_pm_lock_create(int a,int b,const char *n,esp_pm_lock_handle_t **out) {(void)a;(void)b;(void)n;stage_count++;if(fail_stage==stage_count)return ESP_FAIL;*out=(esp_pm_lock_handle_t *)handle();pm_live++;return ESP_OK;}
static esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t *p) {(void)p;stage_count++;if(fail_stage==stage_count)return ESP_FAIL;pm_acquired++;return ESP_OK;}
static esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t *p) {(void)p;if(pm_acquired)pm_acquired--;return ESP_OK;}
static esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t *p) {(void)p;if(pm_live)pm_live--;return ESP_OK;}
static void psa_destroy_key(psa_key_id_t k) {(void)k;}
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { alloc_calls++;if(alloc_calls==fail_alloc_at)return NULL;live_alloc++;return handle(); }
static SemaphoreHandle_t xSemaphoreCreateCounting(unsigned max,unsigned initial) {(void)max;(void)initial;alloc_calls++;if(alloc_calls==fail_alloc_at)return NULL;live_alloc++;return handle();}
static QueueHandle_t xQueueCreate(unsigned n,unsigned size) {(void)n;(void)size;alloc_calls++;if(alloc_calls==fail_alloc_at)return NULL;live_alloc++;return handle();}
static QueueHandle_t xQueueCreateWithCaps(unsigned n,unsigned size,unsigned caps) {assert(caps==3);return xQueueCreate(n,size);}
static void vQueueDelete(QueueHandle_t q) {(void)q;if(live_alloc)live_alloc--;}
static void vQueueDeleteWithCaps(QueueHandle_t q) {vQueueDelete(q);}
static void vSemaphoreDelete(SemaphoreHandle_t s) {(void)s;if(live_alloc)live_alloc--;}
static int xSemaphoreTake(SemaphoreHandle_t s,uint32_t t) {(void)s;(void)t;return 1;}
static int xSemaphoreGive(SemaphoreHandle_t s) {(void)s;gate_gives++;return 1;}
static void vTaskDelete(TaskHandle_t t) {(void)t;if(task_live)task_live--;task_deletes++;}
static int xTaskCreatePinnedToCore(TaskFunction_t fn,const char *n,unsigned stack,void *arg,unsigned prio,TaskHandle_t *out,int core) {(void)fn;(void)n;(void)stack;(void)arg;(void)prio;(void)core;stage_count++;if(fail_stage==stage_count)return 0;*out=handle();task_live++;return pdPASS;}
static void hub_state_init(hub_state_t *s) {(void)s;}
static uint64_t random_session(void) {return 42;}
static uint32_t hid_link_version(void) {return 3;}
static int hid_link_transport(void) {return 0;}
static int hid_link_state(void) {return 0;}
static void hid_link_set_router(void *f) {(void)f;}
static void forward(void) {}
static void worker(void *p) {(void)p;}
static void heartbeat_task(void *p) {(void)p;}
static int is_hub(void) {return s_config.role==HID_HUB_HUB;}
static void receive_frame(const esp_now_recv_info_t *i,const uint8_t *d,int n) {(void)i;(void)d;(void)n;}
static esp_err_t add_radio_peer(const uint8_t *m) {(void)m;stage_count++;return fail_stage==stage_count?ESP_FAIL:ESP_OK;}
'''

suffix = r'''
 static void reset_case(void) {
 memset(&s_config,0,sizeof(s_config)); memset(s_keys,0,sizeof(s_keys)); memset(&s_state,0,sizeof(s_state));
 s_lock=s_sender=s_start_gate=NULL; s_rx=s_local=s_ack=NULL; s_worker_task=s_heartbeat_task=NULL; s_power=NULL;
 s_bus_open=s_starting=false; atomic_store(&s_started,false); fail_alloc_at=alloc_calls=fail_stage=stage_count=live_alloc=radio_live=bus_live=pm_live=pm_acquired=task_live=task_deletes=gate_gives=0;
 next_handle=1; imported_keys=0; family_present=false;
}
static void assert_clean(void) {
 if(live_alloc||radio_live||bus_live||pm_live||pm_acquired||task_live||atomic_load(&s_started)||gate_gives) fprintf(stderr,"leak alloc=%d radio=%d bus=%d pm=%d acquired=%d tasks=%d started=%d gates=%d stage=%d fail=%d allocpoint=%d\n",live_alloc,radio_live,bus_live,pm_live,pm_acquired,task_live,atomic_load(&s_started),gate_gives,stage_count,fail_stage,fail_alloc_at);
 assert(!live_alloc); assert(!radio_live); assert(!bus_live); assert(!channel_live); assert(!pm_live); assert(!pm_acquired); assert(!task_live); assert(!atomic_load(&s_started)); assert(gate_gives==0);
}
int main(void) {
 uint8_t self[6]={2,1,2,3,4,5}, peer[6]={4,1,2,3,4,5}, a[24], b[24];
 assert(hid_ecos_context(self,peer,a)&&hid_ecos_context(peer,self,b)&&!memcmp(a,b,24));
 assert(!memcmp(a,"RHUB-ECOS-v1",12)); assert(!hid_ecos_context(self,self,a));
 uint8_t store[HID_ECOS_STORE]={1,1}; memcpy(store+2,peer,6); memset(store+32,7,32);
 assert(hid_ecos_store(store,sizeof(store))); assert(hid_ecos_find(store,peer)==store+2);
 assert(!hid_ecos_store(store,sizeof(store)-1)); store[1]=7; assert(!hid_ecos_store(store,sizeof(store)));
 store[1]=2; memcpy(store+2+HID_ECOS_RECORD,store+2,HID_ECOS_RECORD); assert(!hid_ecos_store(store,sizeof(store)));
 store[1]=1; store[2]|=1; assert(!hid_ecos_store(store,sizeof(store))); store[2]&=254;
 memset(store+32,0,32); assert(!hid_ecos_store(store,sizeof(store)));
 for(int fail=1;fail<=6;fail++) { reset_case(); fail_alloc_at=fail; (void)hid_hub_start("Test"); assert_clean(); }
 /* fail radio acquire, channel selection, bus open, PM create, PM acquire, worker creation */
 for(int fail=1;fail<=7;fail++) { reset_case(); fail_stage=fail; (void)hid_hub_start("Test"); assert_clean(); }
 /* Existing peer key import and peer registration failures also unwind. */
 reset_case(); s_config.peers[0].used=1; fail_stage=5; (void)hid_hub_start("Test"); assert_clean();
 reset_case(); s_config.peers[0].used=1; fail_stage=6; (void)hid_hub_start("Test"); assert_clean();
 /* Revoked or rotated ECOS credentials cannot reuse a stored HID grant. */
 reset_case(); s_config.peers[0].used=2; fail_stage=5; (void)hid_hub_start("Test"); assert_clean(); assert(imported_keys==0);
 reset_case(); s_config.peers[0].used=2; family_present=true; fail_stage=5; (void)hid_hub_start("Test"); assert_clean(); assert(imported_keys==0);
 reset_case(); s_config.peers[0].used=2; memset(s_config.peers[0].key,3,16); family_present=true; fail_stage=7; (void)hid_hub_start("Test"); assert_clean(); assert(imported_keys==1);
 /* satellite adds a heartbeat task; its failure must delete the already-created gated worker */
 reset_case(); s_config.schema=2; s_config.role=HID_HUB_SATELLITE; fail_stage=8; (void)hid_hub_start("Test"); assert_clean(); assert(task_deletes==1);
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="hid-hub-start-") as td:
    td = Path(td)
    source_file = td / "test.c"
    obj = td / "obj"
    obj.mkdir()
    source_file.write_text('#include "' + (test_dir.parent / 'hid_hub_ecos.h').as_posix() + '"\n' +
                           prefix + production_start + suffix, encoding="utf-8")
    batch = td / "run.cmd"
    exe = td / "test.exe"
    vcvars = Path("C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat")
    batch.write_text(f'@echo off\ncall "{vcvars}" >nul && cl /nologo /std:c11 /W3 "{source_file}" /Fo:"{obj.as_posix()}/" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"\n', encoding="utf-8")
    subprocess.run(["cmd.exe", "/d", "/c", str(batch)], check=True)
print("PASS: extracted production hid_hub_start under allocation/radio/bus/PM/task fault injection")
