#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))

static int fail_calloc, alloc_count, free_count, fail_post, fail_pair_start;
static void *heap_caps_calloc(size_t n, size_t size, int caps)
{
    (void)caps;
    if (fail_calloc) return NULL;
    void *p = calloc(n, size);
    if (p) ++alloc_count;
    return p;
}
static void heap_caps_free(void *p)
{
    if (p) { ++free_count; free(p); }
}

typedef void (*host_job_t)(void *);
static host_job_t posted_job;
static void *posted_arg;
static bool sys_post(host_job_t fn, void *arg)
{
    if (fail_post) return false;
    posted_job = fn;
    posted_arg = arg;
    return true;
}

typedef bool (*ecos_pair_ble_send_t)(const uint8_t *, size_t, void *);
static ecos_pair_ble_send_t service_send;
static void *service_context;
static int service_starts, service_receives, service_cancels;
static uint8_t received_frame[122];
static size_t received_length;
static bool block_receive;
static HANDLE receive_entered, receive_release;
static esp_err_t ecos_pair_ble_start(ecos_pair_ble_send_t send, void *context)
{
    ++service_starts;
    if (fail_pair_start) return fail_pair_start;
    service_send = send;
    service_context = context;
    return ESP_OK;
}
static bool ecos_pair_ble_receive(const uint8_t *frame, size_t length, void *context)
{
    if (context != service_context || frame == NULL || length > sizeof(received_frame)) return false;
    ++service_receives;
    received_length = length;
    memcpy(received_frame, frame, length);
    if (block_receive)
    {
        SetEvent(receive_entered);
        WaitForSingleObject(receive_release, INFINITE);
    }
    return true;
}
static void ecos_pair_ble_disconnect(void *context)
{
    if (context == service_context) ++service_cancels;
}

static unsigned delay_calls;
static void host_delay(void);
