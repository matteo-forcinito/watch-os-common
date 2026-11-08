#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ble_ecos_bus.h"
#include "ble_ecos_bus_stubs/services/ecos.h"
#include "mbedtls/base64.h"

static const char b64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64v(unsigned char c){const char *p=strchr(b64,c);return p?(int)(p-b64):-1;}
int mbedtls_base64_encode(unsigned char *dst,size_t cap,size_t *out,const unsigned char *src,size_t n){size_t need=((n+2)/3)*4;if(cap<need)return -1;size_t k=0;for(size_t i=0;i<n;i+=3){uint32_t v=(uint32_t)src[i]<<16;if(i+1<n)v|=(uint32_t)src[i+1]<<8;if(i+2<n)v|=src[i+2];dst[k++]=b64[(v>>18)&63];dst[k++]=b64[(v>>12)&63];dst[k++]=i+1<n?b64[(v>>6)&63]:'=';dst[k++]=i+2<n?b64[v&63]:'=';}*out=k;return 0;}
int mbedtls_base64_decode(unsigned char *dst,size_t cap,size_t *out,const unsigned char *src,size_t n){if(n%4)return -1;size_t pad=n&&src[n-1]=='='?1:0;if(n>1&&src[n-2]=='=')pad++;size_t need=(n/4)*3-pad;if(need>cap)return -1;size_t k=0;for(size_t i=0;i<n;i+=4){int a=b64v(src[i]),b=b64v(src[i+1]),c=src[i+2]=='='?0:b64v(src[i+2]),d=src[i+3]=='='?0:b64v(src[i+3]);if(a<0||b<0||c<0||d<0)return -1;uint32_t v=((uint32_t)a<<18)|((uint32_t)b<<12)|((uint32_t)c<<6)|(uint32_t)d;if(k<need)dst[k++]=(uint8_t)(v>>16);if(k<need)dst[k++]=(uint8_t)(v>>8);if(k<need)dst[k++]=(uint8_t)v;}*out=k;return 0;}

static int fail_alloc, allocs, frees, fail_mutex, fail_open, receive_count;
static int fail_line, line_count, current_epoch;
static unsigned char sent_frame[205], received_frame[205]; static size_t sent_len, received_len;
static ecos_ble_send_t family_send; static void *family_context;
static int family_opened, family_closed, family_active;
static HANDLE callback_entered, callback_release, callback_done, close_entered;
static int block_line;
static CRITICAL_SECTION family_lock;

void *heap_caps_calloc(size_t n,size_t s,int caps) { (void)caps;if(fail_alloc)return NULL;void *p=calloc(n,s);if(p)allocs++;return p; }
void heap_caps_free(void *p) { if(p){frees++;free(p);} }
HANDLE xSemaphoreCreateMutex(void) { return fail_mutex?NULL:CreateMutexA(NULL,FALSE,NULL); }
int xSemaphoreTake(HANDLE h,unsigned timeout) { return WaitForSingleObject(h,timeout)==WAIT_OBJECT_0; }
int xSemaphoreGive(HANDLE h) { return ReleaseMutex(h); }

bool ecos_family_ble_open(ecos_ble_send_t send,void *context,const uint8_t mac[6]) {
    assert(mac[0]==0x12&&mac[5]==0xbc); if(fail_open)return false;
    family_send=send;family_context=context;family_opened++;return true;
}
bool ecos_family_ble_receive(const uint8_t *frame,size_t length,void *context) {
    if(context!=family_context)return false; receive_count++;received_len=length;memcpy(received_frame,frame,length);return true;
}
void ecos_family_ble_close(void *context) {
    assert(context==family_context);SetEvent(close_entered);
    EnterCriticalSection(&family_lock);
    while(family_active) { LeaveCriticalSection(&family_lock); WaitForSingleObject(callback_done,INFINITE); ResetEvent(callback_done); EnterCriticalSection(&family_lock); }
    family_context=NULL;family_send=NULL;family_closed++;LeaveCriticalSection(&family_lock);
}

static bool send_line(const char *line,unsigned epoch) {
    assert(strstr(line,"\"t\":\"ecosbus\""));line_count++;current_epoch=(int)epoch;
    if(block_line){SetEvent(callback_entered);WaitForSingleObject(callback_release,INFINITE);}
    if(fail_line)return false;
    const char *d=strstr(line,"\"d\":\""); if(!d)return true; d+=5;
    const char *end=strchr(d,'"'); assert(end);
    size_t encoded=(size_t)(end-d), out=0;
    assert(mbedtls_base64_decode(sent_frame,sizeof(sent_frame),&out,(const unsigned char *)d,encoded)==0);
    sent_len=out;return true;
}
static cJSON *req(const char *q,const char *action,const char *mac,const char *data) {
    cJSON *j=cJSON_CreateObject(); assert(j);cJSON_AddStringToObject(j,"q",q);cJSON_AddStringToObject(j,"action",action);
    if(mac)cJSON_AddStringToObject(j,"mac",mac);if(data)cJSON_AddStringToObject(j,"d",data);return j;
}
static void call(cJSON *j,unsigned epoch) { ble_ecos_bus_request(j,epoch);cJSON_Delete(j); }
static const char *Q="0123456789abcdef", *MAC="1234567890bc";
static const char *open_req(void) { static char j[96];snprintf(j,sizeof(j),"{\"q\":\"%s\",\"action\":\"open\",\"mac\":\"%s\"}",Q,MAC);return j; }
static DWORD WINAPI frame_thread(void *arg) {
    (void)arg; EnterCriticalSection(&family_lock);family_active++;LeaveCriticalSection(&family_lock);SetEvent(callback_entered);
    uint8_t frame[49]={0}; bool ok=family_send(frame,sizeof(frame),family_context);assert(ok);
    EnterCriticalSection(&family_lock);family_active--;if(!family_active)SetEvent(callback_done);LeaveCriticalSection(&family_lock);return 0;
}
static DWORD WINAPI disconnect_thread(void *arg) {(void)arg;ble_ecos_bus_disconnect(44);return 0;}

int main(void) {
    InitializeCriticalSection(&family_lock);callback_entered=CreateEventA(NULL,TRUE,FALSE,NULL);callback_release=CreateEventA(NULL,TRUE,FALSE,NULL);callback_done=CreateEventA(NULL,TRUE,FALSE,NULL);close_entered=CreateEventA(NULL,TRUE,FALSE,NULL);assert(callback_entered&&callback_release&&callback_done&&close_entered);
    fail_mutex=1;ble_ecos_bus_init(send_line);call(cJSON_Parse(open_req()),44);assert(family_opened==0);fail_mutex=0;
    ble_ecos_bus_init(send_line);
    cJSON *j=cJSON_Parse(open_req());call(j,44);assert(family_opened==1&&allocs==1&&frees==0);
    call(req("fedcba9876543210","open",MAC,NULL),44);assert(allocs==1&&family_opened==1);
    /* foreign epoch and q are ignored; a valid request forwards decoded bounded bytes. */
    call(req(Q,"frame",NULL,"AQID"),43);call(req("fedcba9876543210","frame",NULL,"AQID"),44);assert(receive_count==0);
    call(req(Q,"frame",NULL,"AQID"),44);assert(receive_count==1&&received_len==3&&received_frame[2]==3);
    /* Encoded values beyond 276 bytes and decoded values beyond 205 are rejected. */
    char huge[278];memset(huge,'A',sizeof(huge)-1);huge[sizeof(huge)-1]=0;
    call(req(Q,"frame",NULL,huge),44);assert(receive_count==1);
    uint8_t max_inbound[205]={0};unsigned char encoded[280];size_t encoded_len=0;
    assert(mbedtls_base64_encode(encoded,sizeof(encoded),&encoded_len,max_inbound,sizeof(max_inbound))==0);encoded[encoded_len]=0;
    call(req(Q,"frame",NULL,(char *)encoded),44);assert(receive_count==2&&received_len==205);
    uint8_t too_big[207]={0};
    assert(mbedtls_base64_encode(encoded,sizeof(encoded),&encoded_len,too_big,sizeof(too_big))==0);encoded[encoded_len]=0;
    call(req(Q,"frame",NULL,(char *)encoded),44);assert(receive_count==2);
    /* Outbound frame bounds and epoch binding. */
    uint8_t out[206]={0};assert(!family_send(out,48,family_context));assert(family_send(out,49,family_context));assert(sent_len==49&&current_epoch==44);
    assert(family_send(out,205,family_context));assert(sent_len==205);assert(!family_send(out,206,family_context));
    /* Failed open allocates then frees context; busy/foreign context remain inert. */
    ble_ecos_bus_disconnect(45);assert(family_closed==0&&frees==0);
    ble_ecos_bus_disconnect(44);assert(family_closed==1&&frees==1);
    fail_line=1;call(req("fedcba9876543210","open",MAC,NULL),44);fail_line=0;assert(allocs==2&&frees==2&&family_closed==2);
    fail_open=1;call(req("fedcba9876543210","open",MAC,NULL),44);fail_open=0;assert(allocs==3&&frees==3);
    fail_alloc=1;call(req("fedcba9876543210","open",MAC,NULL),44);fail_alloc=0;assert(frees==3);
    /* Reopen and prove close drains active callback references before freeing. */
    call(cJSON_Parse(open_req()),44);assert(family_opened==3&&allocs==4&&frees==3);
    block_line=1; HANDLE tx=CreateThread(NULL,0,frame_thread,NULL,0,NULL);assert(tx);assert(WaitForSingleObject(callback_entered,5000)==WAIT_OBJECT_0);
    HANDLE rx=CreateThread(NULL,0,disconnect_thread,NULL,0,NULL);assert(rx);assert(WaitForSingleObject(close_entered,5000)==WAIT_OBJECT_0);
    assert(frees==3);SetEvent(callback_release);assert(WaitForSingleObject(tx,5000)==WAIT_OBJECT_0);assert(WaitForSingleObject(rx,5000)==WAIT_OBJECT_0);CloseHandle(tx);CloseHandle(rx);
    assert(frees==4&&family_closed==3&&family_context==NULL);
    DeleteCriticalSection(&family_lock);CloseHandle(callback_entered);CloseHandle(callback_release);CloseHandle(callback_done);CloseHandle(close_entered);
    puts("PASS: production ECOS BLE bus bridge epochs, bounds, failures and close drain");return 0;
}
