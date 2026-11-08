#include "../link_family.h"
#include "services/ecos.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ecos_family_t family;
static int holds, calls, allocations, freed, mode;
static bool fail_alloc;
static int64_t now_us;
void *heap_caps_calloc(size_t n, size_t size, int caps)
{ (void)caps; if (fail_alloc) return NULL; ++allocations; return calloc(n, size); }
void heap_caps_free(void *p) { if (p) { ++freed; free(p); } }
void ecos_family(ecos_family_t *out) { *out = family; }
void ecos_family_hold(bool on) { holds += on ? 1 : -1; assert(holds >= 0 && holds <= 1); }
esp_err_t ecos_family_refresh(void) { return 0; }
int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(unsigned ticks) { now_us += (int64_t)ticks * 1000; }
int ecos_family_orbit_large(const char *id, const uint8_t *request, size_t length,
                            uint8_t *reply, size_t max, int timeout_ms)
{
    assert(strcmp(id, "aabbccddeeff") == 0 && holds == 1 && timeout_ms == 10000 && max == 144);
    ++calls;
    cJSON *cmd = cJSON_ParseWithLengthOpts((const char *)request, length, NULL, false);
    assert(cmd); cJSON_Delete(cmd);
    if (mode == 1) return -1;
    const char *text = mode == 2 ? "{\"ok\":false}" : mode == 3 ? "not json" :
        "{\"running\":true,\"ringing\":false,\"total\":60,\"remaining\":59,\"watch\":false,\"elapsed\":0}";
    assert(strlen(text) <= max); memcpy(reply, text, strlen(text)); return (int)strlen(text);
}
#include "../link_family.c"

static cJSON *relay(const char *text, int *status)
{
    cJSON *in = cJSON_Parse(text);
    cJSON *out = link_family_relay(in, status);
    cJSON_Delete(in); assert(holds == 0 && allocations == freed); return out;
}
int main(void)
{
    family.count = 1;
    strcpy(family.members[0].id, "aabbccddeeff"); strcpy(family.members[0].name, "Kitchen");
    family.members[0].known = family.members[0].reachable = true;
    cJSON *out = link_family_snapshot();
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(out, "members")) == 1 && holds == 0);
    cJSON_Delete(out);
    family.busy = true; now_us = 0;
    out = link_family_snapshot(); assert(now_us >= 4000000 && holds == 0); cJSON_Delete(out); family.busy = false;
    int status;
    out = relay("{}", &status); assert(status == 400 && calls == 0); cJSON_Delete(out);
    out = relay("{\"id\":\"112233445566\",\"p\":\"/api/timer\"}", &status);
    assert(status == 404 && calls == 0); cJSON_Delete(out);
    out = relay("{\"id\":\"aabbccddeeff\",\"p\":\"/api/system\"}", &status);
    assert(status == 200 && calls == 0 && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(out, "name")), "Kitchen") == 0); cJSON_Delete(out);
    out = relay("{\"id\":\"aabbccddeeff\",\"p\":\"/api/apps/open\"}", &status);
    assert(status == 404 && calls == 0); cJSON_Delete(out);
    const char *timer = "{\"id\":\"aabbccddeeff\",\"p\":\"/api/timer\",\"b\":{\"action\":\"start\",\"seconds\":60}}";
    out = relay(timer, &status); assert(status == 200 && calls == 1 && cJSON_GetObjectItem(out, "countdown")); cJSON_Delete(out);
    mode = 1; out = relay(timer, &status); assert(status == 504 && calls == 2); cJSON_Delete(out);
    mode = 2; out = relay(timer, &status); assert(status == 409 && calls == 3); cJSON_Delete(out);
    mode = 3; out = relay(timer, &status); assert(status == 504 && calls == 4); cJSON_Delete(out);
    fail_alloc = true; out = relay(timer, &status); assert(status == 503 && calls == 4); cJSON_Delete(out);
    assert(link_family_snapshot() == NULL && holds == 0);
    puts("Family gateway membership, bounds, leases and unknown outcomes passed");
    return 0;
}
