#include "../link_orbit.h"
#include "../link_sensor.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int scenes, house_scenes, house_timers, timer_calls, memo_calls;
static bool memo_available;
static bool memo_failure;
static bool memo_maximum;
static bool intercom(const char *from, const cJSON *json)
{
    assert(strcmp(from, "001122334455") == 0);
    return cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json, "session"));
}
static cJSON *info(void)
{
    return cJSON_Parse("{\"w\":466,\"h\":466,\"round\":true,\"voice\":true,\"knob\":false}");
}
static cJSON *sensor(const char *part)
{
    const float axes[3] = {1, 2, 3};
    return link_sensor_json(part, true, true, true, 10, axes, axes, 25);
}
static uint32_t fake_latest = 40, fake_first = 9;

static bool scene(cJSON *json)
{
    scenes++;
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "scene")), "pulse") == 0);
    cJSON_Delete(json);
    return true;
}

static bool house_scene(cJSON *json, bool direct)
{
    house_scenes++;
    assert(direct);
    assert(cJSON_GetObjectItemCaseSensitive(json, "op") == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(json, "direct") == NULL);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "scene")), "aura") == 0);
    cJSON_Delete(json);
    return true;
}

static bool house_timer(cJSON *json, bool direct)
{
    house_timers++;
    assert(direct);
    assert(cJSON_GetObjectItemCaseSensitive(json, "op") == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(json, "direct") == NULL);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "action")), "stop") == 0);
    cJSON_Delete(json);
    return true;
}

static bool timer(const char *action, int seconds, link_orbit_timer_state_t *state)
{
    timer_calls++;
    if (action != NULL && strcmp(action, "start") != 0) return false;
    if (action != NULL && (seconds < 1 || seconds > 86400)) return false;
    *state = (link_orbit_timer_state_t){.countdown_running = true, .countdown_total_s = 60,
                                        .countdown_remaining_s = 42, .watch_elapsed_ms = 1234};
    return true;
}

static void events(uint32_t since, uint32_t *latest, uint32_t *first, link_orbit_event_t *next)
{
    *latest = fake_latest;
    *first = fake_first;
    uint32_t seq = since >= fake_latest ? 0 : (since < fake_first - 1 ? fake_first : since + 1);
    memset(next, 0, sizeof(*next));
    if (seq != 0 && seq <= fake_latest)
    {
        next->seq = seq;
        strcpy(next->type, "dial");
        next->value = 3;
    }
}

static bool memo_supported(void) { return memo_available; }

static bool memo(const char *action, int index, link_orbit_memo_state_t *state)
{
    memo_calls++;
    if (memo_failure) return false;
    if (action != NULL) return strcmp(action, "delete") == 0 && index == 1;
    *state = (link_orbit_memo_state_t){.phase = 0, .selected = 1, .count = 3,
                                       .elapsed_ms = 40, .duration_ms = 100,
                                       .found = index == 1};
    strcpy(state->name, "memo-20261106-120000.wav");
    if (memo_maximum)
    {
        state->phase = 6;
        state->selected = 19;
        state->count = 20;
        state->elapsed_ms = UINT32_MAX;
        state->duration_ms = UINT32_MAX;
        memset(state->levels, 60, sizeof(state->levels));
    }
    return true;
}

static const link_orbit_ops_t ops = {
    .info = info,
    .sensor = sensor,
    .intercom = intercom,
    .scene = scene, .house_scene = house_scene, .house_timer = house_timer,
    .timer = timer, .events = events,
    .memo_supported = memo_supported, .memo = memo,
};

static size_t call(const char *request, uint8_t *reply, size_t cap)
{
    return link_orbit_dispatch("001122334455", (const uint8_t *)request, strlen(request), reply, cap);
}

static cJSON *reply_json(const uint8_t *reply, size_t length)
{
    return cJSON_ParseWithLengthOpts((const char *)reply, length, NULL, false);
}

int main(void)
{
    link_orbit_set_ops(&ops);
    uint8_t reply[LINK_ORBIT_MAX + 2] = {0};
    cJSON *json;
    size_t control_length = call("{\"op\":\"intercom\",\"action\":\"listen\",\"session\":\"0000000000000001\"}", reply, LINK_ORBIT_MAX);
    json = reply_json(reply, control_length);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "queued")));
    cJSON_Delete(json);
    const char *control = "{\"op\":\"intercom\",\"session\":\"0000000000000001\"}";
    control_length = link_orbit_dispatch(NULL, (const uint8_t *)control, strlen(control), reply, LINK_ORBIT_MAX);
    json = reply_json(reply, control_length);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "ok")));
    cJSON_Delete(json);
    size_t info_length = call("{\"op\":\"info\"}", reply, LINK_ORBIT_MAX);
    json = reply_json(reply, info_length);
    assert(json && cJSON_GetObjectItemCaseSensitive(json, "w")->valueint == 466);
    cJSON_Delete(json);
    info_length = call("{\"op\":\"sensor\",\"part\":\"accel\"}", reply, LINK_ORBIT_MAX);
    json = reply_json(reply, info_length);
    assert(json && cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "v"), 0)->valueint == 1000);
    cJSON_Delete(json);
    info_length = call("{\"op\":\"sensor\",\"part\":42}", reply, LINK_ORBIT_MAX);
    json = reply_json(reply, info_length);
    assert(json && cJSON_GetObjectItemCaseSensitive(json, "error") != NULL);
    cJSON_Delete(json);

    assert(call("{\"op\":\"scene\",\"scene\":\"pulse\"} trailing", reply, LINK_ORBIT_MAX) == 0);
    assert(call("{\"op\":\"scene\",\"scene\":\"pulse\"", reply, LINK_ORBIT_MAX) == 0);
    assert(scenes == 0);

    size_t n = call("{\"op\":\"scene\",\"scene\":\"pulse\"}", reply, LINK_ORBIT_MAX);
    assert(n == strlen("{\"ok\":true}"));
    assert(memcmp(reply, "{\"ok\":true}", n) == 0 && scenes == 1);
    memset(reply, 0xA5, sizeof(reply));
    assert(call("{\"op\":\"scene\",\"scene\":\"pulse\"}", reply, n - 1) == 0);
    assert(reply[n - 1] == 0xA5 && reply[n] == 0xA5);
    n = call("{\"op\":\"scene\",\"scene\":\"pulse\"}", reply, n);
    assert(n == strlen("{\"ok\":true}"));

    n = call("{\"op\":\"house_scene\",\"direct\":false,\"scene\":\"aura\"}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "ok")) &&
           cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "queued")) && house_scenes == 0);
    cJSON_Delete(json);
    n = call("{\"op\":\"house_scene\",\"direct\":true,\"scene\":\"aura\"}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok")) &&
           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "queued")) && house_scenes == 1);
    cJSON_Delete(json);
    n = call("{\"op\":\"house_timer\",\"direct\":true,\"action\":\"stop\"}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok")) &&
           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "queued")) && house_timers == 1);
    cJSON_Delete(json);

    n = call("{\"op\":\"timer\",\"action\":\"bogus\"}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "ok")) && timer_calls == 1);
    cJSON_Delete(json);
    n = call("{\"op\":\"timer\",\"action\":\"start\",\"seconds\":\"60\"}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "ok")) && timer_calls == 1);
    cJSON_Delete(json);
    n = call("{\"op\":\"timer\",\"action\":\"start\",\"seconds\":60}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "running")) && timer_calls == 2);
    cJSON_Delete(json);

    memo_available = false;
    n = call("{\"op\":\"memo\"}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "error")), "unsupported") == 0);
    assert(memo_calls == 0);
    cJSON_Delete(json);
    memo_available = true;
    n = call("{\"op\":\"memo\",\"action\":\"delete\",\"index\":1}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok")) && memo_calls == 1);
    cJSON_Delete(json);
    memo_failure = true;
    n = call("{\"op\":\"memo\"}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "ok")));
    cJSON_Delete(json);
    memo_failure = false;

    n = call("{\"op\":\"events\",\"since\":0}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "gap")));
    assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
               cJSON_GetObjectItemCaseSensitive(json, "event"), "seq")) == 9);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "more")));
    cJSON_Delete(json);
    n = call("{\"op\":\"events\",\"since\":9}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "gap")));
    assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
               cJSON_GetObjectItemCaseSensitive(json, "event"), "seq")) == 10);
    cJSON_Delete(json);

    fake_first = 4000000000u;
    fake_latest = 4000000005u;
    n = call("{\"op\":\"events\",\"since\":4000000001}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(
               cJSON_GetObjectItemCaseSensitive(json, "event"), "seq")) == 4000000002.0);
    cJSON_Delete(json);
    n = call("{\"op\":\"events\",\"since\":4000000010}", reply, sizeof(reply));
    json = reply_json(reply, n);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "gap")));
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(json, "event")));
    cJSON_Delete(json);

    char too_long[1024 + 2];
    memset(too_long, ' ', sizeof(too_long));
    too_long[0] = '{'; too_long[1024] = '}'; too_long[1024 + 1] = '\0';
    assert(link_orbit_dispatch("id", (const uint8_t *)too_long, 1024 + 1,
                               reply, sizeof(reply)) == 0);
    const uint8_t embedded_nul[] = {'{','"','o','p','"',':','"','t','i','m','e','r','"','}',0,' '};
    assert(link_orbit_dispatch("id", embedded_nul, sizeof(embedded_nul), reply, sizeof(reply)) == 0);

    memo_maximum = true;
    n = call("{\"op\":\"memo\"}", reply, 144);
    json = reply_json(reply, n);
    assert(cJSON_GetObjectItemCaseSensitive(json, "error") == NULL);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "lv")),
                  "3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c") == 0);
    cJSON_Delete(json);
    puts("link Orbit RPC tests passed");
    return 0;
}
