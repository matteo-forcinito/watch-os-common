#include "link_gateway.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    cJSON *body = cJSON_Parse("{\"scene\":\"aura\",\"op\":\"injected\"}");
    cJSON *cmd = link_gateway_command("/api/link/scene", body);
    assert(cmd && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "op")), "scene") == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(body, "op")), "injected") == 0);
    cJSON_Delete(cmd); cJSON_Delete(body);
    assert(!link_gateway_command("/api/link/scene", NULL));
    assert(!link_gateway_command("/api/apps/open", NULL));
    const char *bad[] = {"/api/link/events?since=-1", "/api/link/events?since=4294967296",
                        "/api/link/events?since=1&x=2", "/api/link/events?since="};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); ++i) assert(!link_gateway_command(bad[i], NULL));
    cmd = link_gateway_command("/api/link/events?since=4294967295", NULL);
    assert(cmd && cJSON_GetObjectItem(cmd, "since")->valuedouble == 4294967295.0);
    cJSON_Delete(cmd);
    cmd = link_gateway_command("/api/link/sensors?part=gyro", NULL);
    assert(cmd && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cmd, "part")), "gyro") == 0);
    cJSON_Delete(cmd);
    assert(!link_gateway_command("/api/link/sensors?part=invalid", NULL));
    cmd = link_gateway_command("/api/link/events", NULL);
    assert(cmd && cJSON_GetObjectItem(cmd, "since")->valuedouble == UINT32_MAX);
    cJSON_Delete(cmd);
    cJSON *out = link_gateway_response("/api/timer", cJSON_Parse(
        "{\"running\":true,\"ringing\":false,\"total\":60,\"remaining\":59,\"watch\":false,\"elapsed\":0}"));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(out, "countdown"), "running")));
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(out, "countdown"), "remaining_s")->valueint == 59);
    cJSON_Delete(out);
    out = link_gateway_response("/api/link", cJSON_Parse("{\"w\":466,\"h\":466,\"round\":true,\"voice\":false,\"knob\":true}"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(out, "features")) == 5);
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(out, "screen"), "w")->valueint == 466);
    cJSON_Delete(out);
    out = link_gateway_response("/api/link/events?since=0", cJSON_Parse("{\"seq\":30,\"event\":{\"seq\":9},\"more\":true}"));
    assert(cJSON_GetObjectItem(out, "seq")->valueint == 9);
    cJSON_Delete(out);
    out = link_gateway_response("/api/link/events?since=0", cJSON_Parse("{\"seq\":1,\"event\":{\"seq\":1},\"more\":false}"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(out, "events")) == 1);
    assert(cJSON_GetObjectItem(out, "event") == NULL);
    cJSON_Delete(out);
    out = link_gateway_response("/api/link/events?since=1", cJSON_Parse("{\"seq\":1,\"event\":null}"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(out, "events")) == 0);
    cJSON_Delete(out);
    puts("Orbit gateway adapter tests passed");
    return 0;
}
