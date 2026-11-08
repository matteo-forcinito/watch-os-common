#include "services/ecos_orbit.h"
#include "../ecos_orbit_fragment.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ecos_orbit_fragment_t receiver;
static int calls, actions, fail_call;
static bool timeout_after_first, probe_reentry;
static int64_t now;
static uint64_t random_id;

int64_t esp_timer_get_time(void) { return now; }
void esp_fill_random(void *out, size_t n)
{
    assert(n == sizeof(random_id));
    ++random_id;
    memcpy(out, &random_id, n);
}

int ecos_family_orbit(const char *id, const uint8_t *request, size_t length,
                      uint8_t *reply, size_t max, int timeout_ms)
{
    assert(id != NULL && timeout_ms > 0 && timeout_ms <= 30000);
    ++calls;
    if (fail_call == calls) return -1;
    if (probe_reentry)
    {
        probe_reentry = false;
        uint8_t large[ECOS_ORBIT_REQUEST_MAX] = {1}, answer[144];
        assert(ecos_family_orbit_large(id, large, sizeof(large), answer, sizeof(answer), 1000) == -1);
    }
    if (length < 4 || memcmp(request, "OF01", 4))
    {
        assert(max >= 2);
        memcpy(reply, "ok", 2);
        ++actions;
        return 2;
    }
    ecos_orbit_fragment_result_t result = ecos_orbit_fragment_accept(&receiver, request, length);
    assert(result != ECOS_ORBIT_FRAGMENT_INVALID);
    if (timeout_after_first && calls == 1) now += 1000000;
    if (result == ECOS_ORBIT_FRAGMENT_MORE)
        return (int)ecos_orbit_fragment_ack(reply, max, receiver.used);
    if (result == ECOS_ORBIT_FRAGMENT_READY)
    {
        ++actions;
        memcpy(receiver.reply, "{\"ok\":true}", 11);
        receiver.reply_length = 11;
    }
    assert(receiver.reply_length <= max);
    memcpy(reply, receiver.reply, receiver.reply_length);
    return (int)receiver.reply_length;
}

static void reset(void)
{
    memset(&receiver, 0, sizeof(receiver));
    calls = actions = fail_call = 0;
    timeout_after_first = probe_reentry = false;
    now = 0;
}

int main(void)
{
    uint8_t input[ECOS_ORBIT_REQUEST_MAX], reply[146];
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (uint8_t)i;
    reset();
    probe_reentry = true;
    assert(ecos_family_orbit_large("001122334455", input, sizeof(input), reply, 144, 1000) == 11);
    assert(calls == 9 && actions == 1 && memcmp(receiver.data, input, sizeof(input)) == 0);
    assert(memcmp(reply, "{\"ok\":true}", 11) == 0);
    reset();
    fail_call = 4;
    assert(ecos_family_orbit_large("id", input, sizeof(input), reply, 144, 1000) == -1);
    assert(calls == 4 && actions == 0);
    fail_call = 0;
    assert(ecos_family_orbit_large("id", input, sizeof(input), reply, 144, 1000) == 11);
    assert(actions == 1); /* failed transfer released its sender guard */
    reset();
    timeout_after_first = true;
    assert(ecos_family_orbit_large("id", input, sizeof(input), reply, 144, 500) == -1);
    assert(calls == 1 && actions == 0);
    reset();
    memset(reply, 0xa5, sizeof(reply));
    assert(ecos_family_orbit_large("id", input, sizeof(input), reply, 10, 1000) == -1);
    assert(actions == 1 && reply[0] == 0xa5 && reply[10] == 0xa5);
    reset();
    assert(ecos_family_orbit_large("id", input, 144, reply, 144, 1000) == 2);
    assert(calls == 1 && actions == 1);
    assert(ecos_family_orbit_large("id", input, sizeof(input) + 1, reply, 144, 1000) == -1);
    assert(ecos_family_orbit_large("id", input, sizeof(input), reply, 144, 30001) == -1);
    puts("Orbit large RPC client tests passed");
    return 0;
}
