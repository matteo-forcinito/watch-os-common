"""Execute production BLE pairing reservation/queue lifetime with SDK fault stubs."""
from pathlib import Path
import subprocess
import tempfile

tests = Path(__file__).resolve().parent
source = (tests.parent / 'ecos.c').read_text(encoding='utf-8')

def function(signature):
    begin = source.index(signature)
    brace = source.index('{', begin)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == '{': depth += 1
        elif source[i] == '}':
            depth -= 1
            if depth == 0: return source[begin:i + 1]
    raise AssertionError(signature)

production = '\n'.join(function(s) for s in (
    'static void close_pair_rx(', 'static esp_err_t start_pair(',
    'esp_err_t ecos_pair_ble_start(', 'bool ecos_pair_ble_receive(',
    'void ecos_pair_ble_disconnect('))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef void *QueueHandle_t;
typedef bool (*ecos_pair_ble_send_t)(const uint8_t *,size_t,void *);
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_INVALID_ARG -3
#define SYS_CORE_ANY -1
#define pdTRUE 1
static int critical;
#define portENTER_CRITICAL(m) do { (void)(m);assert(!critical);critical++; } while(0)
#define portEXIT_CRITICAL(m) do { (void)(m);assert(critical==1);critical--; } while(0)
typedef struct { uint8_t mac[6], length, data[ECOS_FRAME_MAX]; } radio_frame_t;
static struct { bool running,comparing,local_ok,remote_ok,saved;uint32_t code;unsigned seconds_left;char candidate[24],candidate_id[13],message[112]; } s_view;
static bool s_busy,s_cancel,s_confirm;
static ecos_pair_ble_send_t s_ble_send;
static void *s_ble_context;
static unsigned s_ble_rx_users;
static QueueHandle_t s_rx;
static int s_guard;
static int fail_task,task_calls,suspend_calls,resume_calls,sends,deletes,delays;
static bool fail_send,finish_inflight;
static uintptr_t task_argument;
static radio_frame_t delivered;
static void worker(void *arg) { (void)arg; }
static void ecos_link_suspend(bool suspend) {
 assert(!critical&&s_busy);if(suspend)suspend_calls++;else resume_calls++;
}
static bool sys_task_create(void (*fn)(void*),const char *name,unsigned stack,void *arg,int priority,int core) {
 assert(fn==worker&&!strcmp(name,"ecos_pair")&&stack==6144&&priority==2&&core==SYS_CORE_ANY);
 assert(!critical&&s_busy);task_calls++;task_argument=(uintptr_t)arg;return !fail_task;
}
static int xQueueSend(QueueHandle_t queue,const void *data,unsigned wait) {
 assert(!critical&&queue==(void*)1&&wait==0&&s_ble_rx_users==1);sends++;delivered=*(const radio_frame_t*)data;return !fail_send;
}
static void vQueueDeleteWithCaps(QueueHandle_t queue) {
 assert(!critical&&queue==(void*)1&&s_rx==NULL&&s_ble_rx_users==0);deletes++;
}
static void vTaskDelay(unsigned ticks) {
 assert(ticks==1&&!critical&&s_rx==NULL&&s_ble_rx_users>0&&finish_inflight);
 delays++;s_ble_rx_users=0;
}
static bool sender(const uint8_t *frame,size_t length,void *context) { (void)frame;(void)length;(void)context;assert(false);return false; }
'''
suffix = r'''
static void reset(void) {
 assert(!critical);memset(&s_view,0,sizeof(s_view));s_busy=s_cancel=s_confirm=false;
 s_ble_send=NULL;s_ble_context=NULL;s_ble_rx_users=0;s_rx=NULL;
 fail_task=task_calls=suspend_calls=resume_calls=sends=deletes=delays=0;
 fail_send=finish_inflight=false;
}
int main(void) {
 reset();assert(ecos_pair_ble_start(NULL,NULL)==ESP_ERR_INVALID_ARG&&task_calls==0);
 fail_task=1;assert(ecos_pair_ble_start(sender,(void*)2)==ESP_ERR_NO_MEM);
 assert(!s_busy&&!s_view.running&&s_ble_send==NULL&&s_ble_context==NULL&&suspend_calls==resume_calls);
 reset();assert(ecos_pair_ble_start(sender,(void*)2)==ESP_OK);
 assert(s_busy&&s_view.running&&s_ble_send==sender&&s_ble_context==(void*)2&&task_argument==UINT32_MAX);
 assert(ecos_pair_ble_start(sender,(void*)3)==ESP_ERR_INVALID_STATE&&task_calls==1&&s_ble_context==(void*)2);
 uint8_t frame[ECOS_HEADER+32]={0};const uint8_t peer[6]={2,1,2,3,4,5},self[6]={4,1,2,3,4,5};
 ecos_wire_header(frame,ECOS_COMMIT,peer,self);
 assert(!ecos_pair_ble_receive(frame,sizeof(frame),(void*)2)); /* Worker has not published RX yet. */
 s_rx=(void*)1;assert(!ecos_pair_ble_receive(NULL,sizeof(frame),(void*)2)&&!ecos_pair_ble_receive(frame,sizeof(frame)-1,(void*)2));
 assert(!ecos_pair_ble_receive(frame,sizeof(frame),(void*)3));
 ecos_pair_ble_disconnect((void*)3);assert(!s_cancel);
 assert(ecos_pair_ble_receive(frame,sizeof(frame),(void*)2)&&sends==1&&s_ble_rx_users==0);
 assert(delivered.length==sizeof(frame)&&!memcmp(delivered.mac,peer,6)&&!memcmp(delivered.data,frame,sizeof(frame)));
 fail_send=true;assert(!ecos_pair_ble_receive(frame,sizeof(frame),(void*)2)&&s_ble_rx_users==0);
 ecos_pair_ble_disconnect((void*)2);assert(s_cancel&&!ecos_pair_ble_receive(frame,sizeof(frame),(void*)2));
 close_pair_rx();assert(deletes==1&&s_rx==NULL);close_pair_rx();assert(deletes==1);
 reset();s_rx=(void*)1;s_ble_rx_users=1;finish_inflight=true;close_pair_rx();assert(delays==1&&deletes==1);
 reset();s_busy=true;ecos_pair_ble_disconnect((void*)2);assert(!s_cancel); /* Never cancel a radio/LAN window. */
 puts("PASS: production BLE EC01 start faults, busy reservation, RX bounds/refcount and teardown drain");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ecos-ble-pair-') as directory:
    directory = Path(directory)
    cfile = directory / 'test.c'
    cfile.write_text('#include "' + (tests.parent / 'ecos_wire.h').as_posix() + '"\n' + prefix + production + suffix)
    exe = directory / 'test.exe'
    batch = directory / 'run.cmd'
    vcvars = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'
    batch.write_text(f'@echo off\ncall "{vcvars}" >nul && cl /nologo /std:c11 /W3 "{cfile}" /Fo:"{directory / "test.obj"}" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"\n')
    subprocess.run(['cmd.exe','/d','/c',str(batch)],check=True)
