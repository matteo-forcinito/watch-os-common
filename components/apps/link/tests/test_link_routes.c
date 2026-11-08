#include "../link_routes.h"
#include "core/sys.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static char stored_key[49], stored_value[64]; static size_t stored_len; static int calls, errors, hold_on, hold_off, fail_post, fail_task, stream, rpc_timeout, reenter;
static bool reject_reply;
static const char *expected_text;
void vTaskDelete(void *task) { assert(task == NULL); }
static void (*pending_fn)(void *); static void *pending_arg;
bool orbit_state_get(const uint8_t *k,size_t n,uint8_t *v,size_t cap,size_t *len,uint64_t *clock,uint8_t author[6]) { (void)clock;(void)author; if(n!=strlen(stored_key)||memcmp(k,stored_key,n)||!stored_len||stored_len>cap)return false;memcpy(v,stored_value,stored_len);*len=stored_len;return true; }
bool orbit_state_put(const uint8_t *k,size_t n,const uint8_t *v,size_t z) { memcpy(stored_key,k,n);stored_key[n]=0;memcpy(stored_value,v,z);stored_len=z;return true; }
bool orbit_state_delete(const uint8_t *k,size_t n) { memcpy(stored_key,k,n);stored_key[n]=0;stored_len=0;return true; }
bool sys_post(void (*fn)(void *),void *arg) { if(fail_post)return false; pending_fn=fn;pending_arg=arg;return true; }
bool sys_task_create(TaskFunction_t fn,const char *name,uint32_t stack,void *arg,UBaseType_t pri,BaseType_t core) { (void)name;(void)pri;(void)core;assert(stack==6144);if(fail_task)return false;fn(arg);return true; }
bool ecos_family_stream_active(void) { return stream!=0; }
void ecos_family_hold(bool on) { if(on)hold_on++;else hold_off++; }
int ecos_family_orbit(const char *id,const uint8_t *req,size_t n,uint8_t *r,size_t m,int timeout)
{
    assert(m >= 12 && timeout == 10000 && strlen(id) == 12 && n < 144);
    char copy[145];
    memcpy(copy, req, n);
    copy[n] = 0;
    cJSON *command = cJSON_Parse(copy);
    assert(command != NULL);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(command, "op")), "input"));
    if (expected_text != NULL)
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(command, "value")), expected_text));
    cJSON_Delete(command);
    const char *answer = reject_reply ? "{\"ok\":false}" : "{\"ok\":true}";
    memcpy(r, answer, strlen(answer));
    calls++;
    if (reenter) { reenter = 0; link_routes_emit("tap", "reentry", 99); }
    return rpc_timeout ? 0 : (int)strlen(answer);
}
void link_event_text(const char *type,const char *text) { assert(!strcmp(type,"route_error"));assert(text);errors++; }

int main(void) {
 assert(!link_routes_init("bad")); assert(link_routes_init("AABBCCDDEEFF"));
 assert(!link_routes_init("001122334455")); assert(!link_routes_init("011122334455"));
 const char *invalid[]={"{\"type\":\"tap\",\"type\":\"dial\"}","{\"type\":\"tap\",\"extra\":1}","{\"type\":\"tap\",\"target\":null,\"target\":null}","{\"type\":\"tap\",\"target\":\"011122334455\"}"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) { cJSON *bad=cJSON_Parse(invalid[i]);assert(link_routes_config(bad)==NULL);cJSON_Delete(bad); }
 cJSON *cfg=cJSON_Parse("{\"type\":\"tap\",\"target\":\"001122334455\"}"); cJSON *out=link_routes_config(cfg); assert(out&&cJSON_IsTrue(cJSON_GetObjectItem(out,"ok"))); cJSON_Delete(out);cJSON_Delete(cfg);
 assert(!strcmp(stored_key,"ecos/route/aabbccddeeff/tap")&&stored_len==12);
 link_routes_emit("tap","x",7); assert(calls==0&&errors==0); pending_fn(pending_arg); assert(calls==1&&errors==0);
 assert(hold_on==1&&hold_off==1);
 cfg=cJSON_Parse("{\"type\":\"tap\",\"target\":\"bad\"}");assert(link_routes_config(cfg)==NULL);cJSON_Delete(cfg);
 cfg=cJSON_Parse("{\"type\":\"tap\",\"target\":null}");out=link_routes_config(cfg);assert(out);cJSON_Delete(out);cJSON_Delete(cfg);assert(stored_len==0);
 cfg=cJSON_Parse("{\"type\":\"tap\",\"target\":\"001122334455\"}");out=link_routes_config(cfg);cJSON_Delete(out);cJSON_Delete(cfg);
 /* Queue capacity and FIFO drain. */
 reenter=1; for(int i=0;i<8;i++) link_routes_emit("tap","q",i);
 link_routes_emit("tap","overflow",9); assert(errors==1);
 pending_fn(pending_arg); assert(calls==10);
 /* sys_post failure discards queued work and reports busy. */
 fail_post=1; link_routes_emit("tap","postfail",10); assert(errors==2); fail_post=0;
 /* task creation failure also drains and reports busy. */
 fail_task=1; link_routes_emit("tap","taskfail",11); pending_fn(pending_arg); assert(errors==3); fail_task=0;
 /* Stream activity declines without holding or disturbing audio. */
 stream=1; link_routes_emit("tap","busy",12); pending_fn(pending_arg); assert(errors==4&&calls==10); stream=0;
 /* Unknown timeout gets one RPC call and releases family hold. */
 rpc_timeout=1; link_routes_emit("tap","timeout",13); pending_fn(pending_arg); assert(errors==5&&calls==11); assert(hold_on==11&&hold_off==11);
 rpc_timeout=0; reject_reply=true; link_routes_emit("tap","rejected",1);pending_fn(pending_arg);assert(errors==6&&calls==12);
 reject_reply=false; expected_text="quote\"\\\n";link_routes_emit("tap",expected_text,2);pending_fn(pending_arg);assert(errors==6&&calls==13&&hold_on==hold_off);
 return 0;
}
