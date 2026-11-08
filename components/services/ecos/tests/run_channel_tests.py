"""Test the production family park/announcement policy against SDK faults."""
from pathlib import Path
import subprocess
import tempfile

tests = Path(__file__).resolve().parent
source = (tests.parent / 'ecos_link.c').read_text(encoding='utf-8')
def extract(signature):
    start = source.index(signature)
    body = source.index('{', start)
    depth = 0
    for index in range(body, len(source)):
        if source[index] == '{': depth += 1
        elif source[index] == '}':
            depth -= 1
            if depth == 0: return source[start:index + 1]
    raise AssertionError(signature)

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "ecos_stream_slot.h"
static int s_lock, depth, writes, saves, tunes;
static bool joined, present, busy, tune_ok=true, save_ok=true;
static uint8_t pinned, home=6, common=1, shared;
static ecos_stream_slot_t s_stream;
static struct {uint8_t mac[6], channel;} s_info[1];
#define portENTER_CRITICAL(lock) ((void)(lock), ++depth)
#define portEXIT_CRITICAL(lock) ((void)(lock), --depth)
static uint8_t wifi_radio_pinned_channel(void) {assert(!depth);return pinned;}
static uint8_t wifi_home_channel(void) {assert(!depth);return home;}
static uint8_t wifi_radio_common_channel(void) {assert(!depth);return common;}
static bool wifi_is_connected(void) {assert(!depth);return joined;}
static bool wifi_radio_set_channel(uint8_t channel) {assert(!depth && channel>=1 && channel<=13);tunes++;return tune_ok;}
static bool wifi_radio_common_channel_set(uint8_t channel) {
 assert(!depth);if(!save_ok)return false;if(common!=channel){saves++;common=channel;}return true;
}
static bool ecos_family_stream_active(void) {return s_stream.active;}
static bool orbit_state_get(const uint8_t *key,size_t n,uint8_t *out,size_t cap,size_t *len,void *clock,void *author) {
 (void)clock;(void)author;assert(!depth && n==12 && memcmp(key,"ecos/channel",12)==0 && cap==1);
 if(!present||busy)return false;*out=shared;*len=1;return true;
}
static bool orbit_state_put(const uint8_t *key,size_t n,const uint8_t *value,size_t len) {
 assert(!depth && n==12 && memcmp(key,"ecos/channel",12)==0 && len==1);writes++;shared=*value;present=true;return true;
}
/* PRODUCTION */
int main(void) {
 park();assert(common==6 && shared==6 && writes==1 && saves==1);
 park();assert(writes==1 && saves==1);
 pinned=1;park();assert(common==1 && shared==1 && writes==2 && saves==2);
 pinned=0;s_stream.active=true;s_info[0].channel=11;
 assert(park_channel()==11);park();assert(common==1 && writes==2 && saves==2);
 s_stream.active=false;joined=true;int before=tunes;park();assert(tunes==before && writes==2);
 joined=false;present=false;home=11;tune_ok=false;park();assert(common==1 && writes==2);
 tune_ok=true;save_ok=false;park();assert(common==1 && writes==2);
 save_ok=true;park();assert(common==11 && shared==11 && writes==3 && saves==3);
 busy=true;park();assert(writes==3 && saves==3);busy=false;
 shared=6;present=true;park();assert(common==6 && writes==3 && saves==4);
 assert(depth==0);puts("PASS: family channel pins, stable announcements, network authority and storage faults");return 0;
}
'''
production = extract('static uint8_t park_channel(') + '\n' + extract('static void park(')
with tempfile.TemporaryDirectory(prefix='ecos-channel-') as temp:
    temp = Path(temp)
    path = temp / 'channel.c'
    path.write_text(fixture.replace('/* PRODUCTION */', production), encoding='utf-8')
    exe = temp / 'channel.exe'
    vcvars = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'
    batch = temp / 'run.cmd'
    batch.write_text(f'@echo off\ncall "{vcvars}" >nul && cl /nologo /std:c11 /W3 /I"{tests.parent}" "{path}" /Fo:"{temp / "channel.obj"}" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"\n', encoding='utf-8')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=tests, check=True)
