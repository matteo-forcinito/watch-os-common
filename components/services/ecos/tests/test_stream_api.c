#include "ecos_stream_slot.h"
#include <assert.h>
#include <stdio.h>

typedef void (*ecos_stream_handler_t)(const char *, const uint8_t *, size_t);
static ecos_stream_handler_t s_stream_handler;
static ecos_stream_slot_t s_stream;
static int s_lock, depth;
static int s_stream_gate = 1, gate_depth, pin_count;
static bool pin_failure;
static struct { bool used; uint8_t mac[6], channel; } s_info[1];
#define portMAX_DELAY 0
static void xSemaphoreTake(int gate, int wait) { (void)gate; (void)wait; assert(depth == 0 && gate_depth == 0); ++gate_depth; }
static void xSemaphoreGive(int gate) { (void)gate; assert(depth == 0 && gate_depth == 1); --gate_depth; }
static uint8_t wifi_radio_channel(void) { assert(depth == 0); return 6; }
static bool wifi_radio_channel_reserve(const char *tag, uint8_t channel)
{ assert(depth == 0 && gate_depth == 1 && strcmp(tag, "OA01") == 0 && channel == 6); if(pin_failure) return false; ++pin_count; return true; }
static void wifi_radio_channel_release(const char *tag)
{ assert(depth == 0 && gate_depth == 1 && strcmp(tag, "OA01") == 0 && pin_count == 1); --pin_count; }
static bool s_suspend, s_running, s_listening, disabled;
#define portENTER_CRITICAL(x) ((void)(x), ++depth)
#define portEXIT_CRITICAL(x) ((void)(x), --depth)
static bool family_disabled(void) { assert(depth == 0); return disabled; }
static bool parse_id(const char *id, uint8_t mac[6])
{
    if (id == NULL || (strcmp(id, "peer") && strcmp(id, "other"))) return false;
    memset(mac, 0, 6); mac[0] = strcmp(id, "peer") ? 4 : 2; return true;
}
/* PRODUCTION */
static void received(const char *id, const uint8_t *data, size_t length)
{ (void)id; (void)data; (void)length; }
int main(void)
{
    uint8_t data[ECOS_STREAM_DATA_MAX + 1] = {1};
    assert(!ecos_family_stream_open(NULL));
    disabled = true; assert(!ecos_family_stream_open("peer")); disabled = false;
    s_suspend = true; assert(!ecos_family_stream_open("peer")); s_suspend = false;
    ecos_family_set_stream_handler(received); assert(s_stream_handler == received && depth == 0);
    assert(!ecos_family_stream_open("peer")); /* no active worker yet */
    s_running = s_listening = true;
    pin_failure = true; assert(!ecos_family_stream_open("peer") && pin_count == 0); pin_failure = false;
    assert(ecos_family_stream_open("peer") && !ecos_family_stream_open("other"));
    assert(pin_count == 1);
    s_running = s_listening = false;
    assert(!ecos_family_stream_send("peer", data, 1));
    s_running = true; assert(!ecos_family_stream_send("peer", data, 1));
    s_listening = true;
    assert(!ecos_family_stream_send("other", data, 1));
    assert(!ecos_family_stream_send("peer", data, sizeof(data)));
    assert(ecos_family_stream_send("peer", data, ECOS_STREAM_DATA_MAX));
    data[0] = 2; assert(ecos_family_stream_send("peer", data, 1) && s_stream.data[0] == 2 && s_stream.length == 1);
    s_suspend = true; assert(!ecos_family_stream_send("peer", data, 1)); s_suspend = false;
    disabled = true; assert(!ecos_family_stream_send("peer", data, 1)); disabled = false;
    ecos_family_stream_close("other"); assert(s_stream.active && pin_count == 1);
    ecos_family_stream_close(NULL); assert(s_stream.active);
    ecos_family_stream_close("peer"); assert(!s_stream.active && s_stream.length == 0 && depth == 0 && pin_count == 0);
    assert(!ecos_family_stream_send("peer", data, 1));
    puts("Family lossy stream API policy, lifecycle and bounds passed");
    return 0;
}
