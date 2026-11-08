#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include "ble_ecos_stubs/host_mock.h"
#include "../ble_ecos.c"

static int sent_count, send_ok = 1;
static char sent_lines[16][248];
static unsigned sent_epochs[16];
static HANDLE receive_thread_entered;
static HANDLE terminal_waiting_event;

static bool send_line(const char *line, unsigned epoch)
{
    assert(sent_count < 16);
    strcpy_s(sent_lines[sent_count], sizeof(sent_lines[sent_count]), line);
    sent_epochs[sent_count] = epoch;
    ++sent_count;
    return send_ok != 0;
}

static void host_delay(void)
{
    if (terminal_waiting_event != NULL) SetEvent(terminal_waiting_event);
    Sleep(1);
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64_value(unsigned char c)
{
    const char *p = strchr(b64, c);
    return p ? (int)(p - b64) : -1;
}

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    size_t needed = ((slen + 2) / 3) * 4;
    if (dlen < needed) return -1;
    size_t out = 0;
    for (size_t i = 0; i < slen; i += 3)
    {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < slen) v |= (uint32_t)src[i + 1] << 8;
        if (i + 2 < slen) v |= src[i + 2];
        dst[out++] = b64[(v >> 18) & 63];
        dst[out++] = b64[(v >> 12) & 63];
        dst[out++] = i + 1 < slen ? b64[(v >> 6) & 63] : '=';
        dst[out++] = i + 2 < slen ? b64[v & 63] : '=';
    }
    *olen = out;
    return 0;
}

int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    if ((slen & 3) != 0) return -1;
    size_t padding = slen && src[slen - 1] == '=' ? 1 : 0;
    if (slen > 1 && src[slen - 2] == '=') ++padding;
    size_t needed = (slen / 4) * 3 - padding;
    if (needed > dlen) return -1;
    size_t out = 0;
    for (size_t i = 0; i < slen; i += 4)
    {
        int a = b64_value(src[i]), b = b64_value(src[i + 1]);
        int c = src[i + 2] == '=' ? 0 : b64_value(src[i + 2]);
        int d = src[i + 3] == '=' ? 0 : b64_value(src[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
        uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)c << 6 | (uint32_t)d;
        if (out < needed) dst[out++] = (uint8_t)(v >> 16);
        if (out < needed) dst[out++] = (uint8_t)(v >> 8);
        if (out < needed) dst[out++] = (uint8_t)v;
    }
    *olen = out;
    return 0;
}

static cJSON *request(const char *id, const char *action, const char *data)
{
    cJSON *obj = cJSON_CreateObject();
    assert(obj != NULL);
    cJSON_AddStringToObject(obj, "q", id);
    cJSON_AddStringToObject(obj, "action", action);
    if (data != NULL) cJSON_AddStringToObject(obj, "d", data);
    return obj;
}

static void call_request(const char *id, const char *action, const char *data, unsigned epoch)
{
    cJSON *obj = request(id, action, data);
    ble_ecos_request(obj, epoch);
    cJSON_Delete(obj);
}

static void run_posted(void)
{
    assert(posted_job != NULL);
    host_job_t job = posted_job;
    void *arg = posted_arg;
    posted_job = NULL;
    posted_arg = NULL;
    job(arg);
}

static DWORD WINAPI request_thread(void *arg)
{
    (void)arg;
    call_request("abcdef0123456789", "frame", "AQID", 12);
    return 0;
}

static DWORD WINAPI terminal_thread(void *arg)
{
    assert(service_send(NULL, 0, arg));
    return 0;
}

int main(void)
{
    ble_ecos_init(send_line);
    call_request("ABCDEF0123456789", "start", NULL, 1);
    call_request("abcdef012345678", "start", NULL, 1);
    call_request("abcdef0123456789", "unknown", NULL, 1);
    assert(service_starts == 0 && alloc_count == 0);

    fail_calloc = 1;
    call_request("abcdef0123456789", "start", NULL, 1);
    fail_calloc = 0;
    assert(service_starts == 0);

    fail_pair_start = -21;
    call_request("abcdef0123456789", "start", NULL, 7);
    fail_pair_start = 0;
    assert(posted_job != NULL && alloc_count == 1 && free_count == 0);
    run_posted();
    assert(free_count == 1 && sent_count == 1 && sent_epochs[0] == 7);
    assert(strstr(sent_lines[0], "\"error\":\"busy\"") != NULL);

    fail_post = 1;
    fail_pair_start = -22;
    call_request("abcdef0123456789", "start", NULL, 8);
    fail_pair_start = 0;
    fail_post = 0;
    assert(free_count == 2 && posted_job == NULL);

    call_request("abcdef0123456789", "start", NULL, 12);
    assert(s_pair != NULL && service_context == s_pair && alloc_count == 3);
    call_request("0123456789abcdef", "start", NULL, 44);
    assert(posted_job != NULL);
    run_posted();
    assert(sent_epochs[1] == 44 && strstr(sent_lines[1], "\"error\":\"busy\"") != NULL);
    assert(s_pair != NULL);

    int receives = service_receives;
    call_request("abcdef0123456789", "frame", "AQID", 13);
    call_request("0123456789abcdef", "frame", "AQID", 12);
    assert(service_receives == receives);
    call_request("abcdef0123456789", "frame", "AQID", 12);
    assert(service_receives == receives + 1 && received_length == 3);
    assert(received_frame[0] == 1 && received_frame[1] == 2 && received_frame[2] == 3);

    uint8_t max_frame[122];
    for (size_t i = 0; i < sizeof(max_frame); ++i) max_frame[i] = (uint8_t)i;
    unsigned char encoded[165];
    size_t encoded_len = 0;
    assert(mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_len, max_frame, sizeof(max_frame)) == 0);
    assert(encoded_len == 164);
    encoded[encoded_len] = 0;
    receives = service_receives;
    call_request("abcdef0123456789", "frame", (const char *)encoded, 12);
    assert(service_receives == receives + 1 && received_length == sizeof(max_frame));

    uint8_t over_frame[123] = {0};
    assert(mbedtls_base64_encode(encoded, sizeof(encoded), &encoded_len, over_frame, sizeof(over_frame)) == 0);
    assert(encoded_len == 164);
    encoded[encoded_len] = 0;
    receives = service_receives;
    call_request("abcdef0123456789", "frame", (const char *)encoded, 12);
    assert(service_receives == receives);
    char too_long[166];
    memset(too_long, 'A', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = 0;
    call_request("abcdef0123456789", "frame", too_long, 12);
    assert(service_receives == receives);

    call_request("abcdef0123456789", "cancel", NULL, 13);
    assert(service_cancels == 0);
    call_request("0123456789abcdef", "cancel", NULL, 12);
    assert(service_cancels == 0);
    call_request("abcdef0123456789", "cancel", NULL, 12);
    assert(service_cancels == 1);
    ble_ecos_disconnect(13);
    assert(service_cancels == 1);
    ble_ecos_disconnect(12);
    assert(service_cancels == 2);

    call_request("abcdef0123456789", "start", NULL, 12);
    run_posted();
    receive_entered = CreateEventA(NULL, TRUE, FALSE, NULL);
    receive_release = CreateEventA(NULL, TRUE, FALSE, NULL);
    terminal_waiting_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(receive_entered && receive_release && terminal_waiting_event);
    block_receive = true;
    HANDLE rx = CreateThread(NULL, 0, request_thread, NULL, 0, NULL);
    assert(rx != NULL);
    assert(WaitForSingleObject(receive_entered, 5000) == WAIT_OBJECT_0);
    HANDLE end = CreateThread(NULL, 0, terminal_thread, service_context, 0, NULL);
    assert(end != NULL);
    assert(WaitForSingleObject(terminal_waiting_event, 5000) == WAIT_OBJECT_0);
    assert(s_pair == NULL && free_count == 4);
    SetEvent(receive_release);
    assert(WaitForSingleObject(rx, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(end, 5000) == WAIT_OBJECT_0);
    CloseHandle(rx); CloseHandle(end);
    CloseHandle(receive_entered); CloseHandle(receive_release); CloseHandle(terminal_waiting_event);
    terminal_waiting_event = NULL;
    block_receive = false;
    assert(s_pair == NULL && free_count == 5);
    assert(strstr(sent_lines[sent_count - 1], "\"end\":true") != NULL);
    assert(sent_epochs[sent_count - 1] == 12);
    assert(alloc_count == free_count);

    puts("PASS: EC01 production BLE adapter lifecycle, bounds, epochs and context isolation");
    return 0;
}
