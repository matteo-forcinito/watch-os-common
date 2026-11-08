#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/semphr.h"
#include "services/ecos.h"
#include "core/sys.h"
#include "../orbit_model.h"

static bool fail_alloc, fail_mutex, fail_nvs_open, fail_nvs_read, fail_nvs_commit, fail_post, fail_task;
static bool run_task_inline;
static int alloc_count, free_count, mutex_count, mutex_delete_count, hold_count, rpc_count, task_delete_count;
static int merge_tx_count, largest_rpc_reply;
static uint64_t durable_clock, staged_clock;
static bool has_durable_clock, has_staged_clock;
static int64_t now_us;
static ecos_family_t family_state;
static void (*posted_fn)(void *);
static void *posted_arg;
static bool link_running;
static bool remote_equal_local;
static bool omit_remote_second;
static orbit_model_store_t s_store;
static orbit_model_entry_t *s_entries;

static SemaphoreHandle_t s_lock;

void *heap_caps_calloc(size_t n, size_t size, int caps)
{
    (void)caps;
    if (fail_alloc) return NULL;
    void *p = calloc(n, size);
    if (p) ++alloc_count;
    return p;
}
void heap_caps_free(void *p) { if (p) { ++free_count; free(p); } }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    if (fail_mutex) return NULL;
    memset(storage, 0, sizeof(*storage));
    ++mutex_count;
    return storage;
}
int xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait)
{
    (void)wait;
    if (m->locked) return 0;
    m->locked = true;
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t m) { assert(m->locked); m->locked = false; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t m) { if (m) ++mutex_delete_count; }
esp_err_t esp_read_mac(unsigned char *mac, int type)
{
    (void)type;
    static const uint8_t local[6] = {0x02, 1, 2, 3, 4, 5};
    memcpy(mac, local, sizeof(local));
    return ESP_OK;
}
int64_t esp_timer_get_time(void) { return now_us; }

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle)
{
    (void)mode;
    assert(strcmp(ns, "ecos_orbit") == 0);
    if (fail_nvs_open) return ESP_FAIL;
    *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_u64(nvs_handle_t handle, const char *key, uint64_t *value)
{
    (void)handle;
    assert(strcmp(key, "clock_res") == 0);
    if (fail_nvs_read) return ESP_FAIL;
    if (!has_durable_clock) return ESP_ERR_NVS_NOT_FOUND;
    *value = durable_clock;
    return ESP_OK;
}
esp_err_t nvs_set_u64(nvs_handle_t handle, const char *key, uint64_t value)
{
    (void)handle;
    assert(strcmp(key, "clock_res") == 0);
    staged_clock = value;
    has_staged_clock = true;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (fail_nvs_commit) return ESP_FAIL;
    if (has_staged_clock) { durable_clock = staged_clock; has_durable_clock = true; has_staged_clock = false; }
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }

void ecos_family(ecos_family_t *out) { *out = family_state; }
void ecos_family_hold(bool on) { hold_count += on ? 1 : -1; }
bool ecos_link_running(void) { return link_running; }
static bool streaming;
bool ecos_family_stream_active(void) { return streaming; }
bool ecos_family_hold_active(void)
{
    if (!link_running) return false;
    ++hold_count;
    return true;
}
int ecos_family_orbit(const char *id, const uint8_t *request, size_t length,
                      uint8_t *reply, size_t max, int timeout_ms)
{
    (void)id; (void)timeout_ms;
    assert(s_lock == NULL || !s_lock->locked);
    ++rpc_count;
    assert(length >= 6 && memcmp(request, "OS01", 4) == 0);
    if (request[4] == 1)
    {
        ++merge_tx_count;
        assert(max >= 6);
        memcpy(reply, "OS01", 4); reply[4] = 1; reply[5] = 1;
        if (largest_rpc_reply < 6) largest_rpc_reply = 6;
        return 6;
    }
    assert(request[4] == 2 || request[4] == 3);
    uint8_t index = request[5];
    orbit_model_entry_t remote = {0};
    uint8_t count = 0;
    if (remote_equal_local)
    {
        count = (uint8_t)s_store.count;
        if (omit_remote_second && count > 0) --count;
        size_t target = index;
        if (omit_remote_second && index >= 1) ++target;
        size_t seen = 0;
        for (size_t i = 0; i < s_store.capacity; ++i)
            if (s_entries[i].used && seen++ == target) { remote = s_entries[i]; break; }
    }
    bool present = index < count && remote.used;
    memcpy(reply, "OS01", 4); reply[4] = request[4]; reply[5] = count; reply[6] = index;
    if (request[4] == 3)
    {
        size_t n = 8;
        reply[7] = present ? (uint8_t)(1 | (remote.tombstone ? 2 : 0)) : 0;
        if (present)
        {
            reply[n++] = remote.key_len;
            memcpy(reply + n, remote.key, remote.key_len); n += remote.key_len;
            for (int i = 7; i >= 0; --i) reply[n++] = (uint8_t)(remote.clock >> (8 * i));
            memcpy(reply + n, remote.author, 6); n += 6;
        }
        if (largest_rpc_reply < (int)n) largest_rpc_reply = (int)n;
        assert(n <= max);
        return (int)n;
    }
    reply[7] = present ? 1 : 0;
    if (!present) return 8;
    size_t n = orbit_model_encode(&remote, reply + 8, max - 8);
    assert(n > 0 && n + 8 <= max);
    n += 8;
    if (largest_rpc_reply < (int)n) largest_rpc_reply = (int)n;
    return (int)n;
}
bool sys_post(void (*fn)(void *), void *arg)
{
    if (fail_post) return false;
    posted_fn = fn; posted_arg = arg;
    return true;
}
bool sys_task_create(void (*fn)(void *), const char *name, int stack,
                     void *arg, int priority, int core)
{
    (void)name; (void)stack; (void)priority; (void)core;
    if (fail_task) return false;
    if (run_task_inline) fn(arg);
    return true;
}
void vTaskDelete(void *task) { assert(task == NULL); ++task_delete_count; }

#include "../orbit_state.c"

static void reset_service(void)
{
    if (s_entries) heap_caps_free(s_entries);
    if (s_lock) vSemaphoreDelete(s_lock);
    memset(&s_store, 0, sizeof(s_store));
    s_entries = NULL; s_lock = NULL; s_ready = false;
    s_sync_pending = false; s_peer_cursor = 0; s_last_sync_us = -ORBIT_SYNC_INTERVAL_US; s_reserved_ceiling = 0;
}

static void test_init_cleanup_and_durable_reservations(void)
{
    int before_alloc = alloc_count, before_free = free_count;
    fail_alloc = true;
    assert(!orbit_state_init());
    assert(!s_ready && alloc_count == before_alloc && free_count == before_free);
    fail_alloc = false;
    fail_nvs_open = true;
    assert(!orbit_state_init());
    assert(!s_ready && free_count == before_free + 1);
    fail_nvs_open = false;
    fail_nvs_read = true;
    int before_mutex_delete = mutex_delete_count;
    assert(!orbit_state_init());
    assert(!s_ready && mutex_delete_count == before_mutex_delete + 1);
    fail_nvs_read = false;
    assert(orbit_state_init());
    static const uint8_t key[] = {0, 'x'}, value[] = {0xff, 0, 3};
    assert(orbit_state_put(key, sizeof(key), value, sizeof(value)));
    assert(durable_clock == 256 && s_store.observed_clock == 1);
    uint8_t out[4] = {0xaa, 0xaa, 0xaa, 0xaa};
    size_t out_len = 0;
    uint64_t version = 0;
    uint8_t author[6];
    assert(!orbit_state_get(key, sizeof(key), out, 2, &out_len, &version, author));
    assert(out[0] == 0xaa && out_len == 0);
    assert(orbit_state_get(key, sizeof(key), out, sizeof(out), &out_len, &version, author));
    assert(out_len == sizeof(value) && version == 1 && memcmp(out, value, sizeof(value)) == 0);
    s_lock->locked = true;
    out_len = 77;
    assert(!orbit_state_get(key, sizeof(key), out, sizeof(out), &out_len, &version, author));
    assert(out_len == 77);
    s_lock->locked = false;

    /* A received clock is reserved before it enters the store. */
    orbit_model_entry_t remote = {0};
    remote.used = true; remote.key_len = 1; remote.key[0] = 'r'; remote.clock = 1000;
    remote.author[5] = 9; remote.value_len = 1; remote.value[0] = 'R';
    assert(merge_entry(&remote));
    assert(durable_clock == 1255 && s_store.observed_clock == 1000);

    reset_service();
    assert(orbit_state_init());
    assert(s_store.observed_clock == 1255);
    assert(orbit_state_put(key, sizeof(key), value, sizeof(value)));
    assert(orbit_state_get(key, sizeof(key), out, sizeof(out), &out_len, &version, author));
    assert(version == 1256 && durable_clock == 1511);
}

static void test_rpc_validation_and_commit_failure(void)
{
    uint8_t request[ECOS_ORBIT_MAX] = {'O', 'S', '0', '1', 1};
    uint8_t reply[ECOS_ORBIT_MAX];
    orbit_model_entry_t incoming = {0};
    incoming.used = true; incoming.key_len = 1; incoming.key[0] = 'z';
    incoming.value_len = 2; incoming.value[0] = 0; incoming.value[1] = 0xfe;
    incoming.clock = 3000; incoming.author[5] = 3;
    size_t encoded = orbit_model_encode(&incoming, request + 5, sizeof(request) - 5);
    assert(encoded == 22);
    fail_nvs_commit = true;
    assert(orbit_state_handle_rpc("000000000001", request, 5 + encoded, reply, sizeof(reply)) == 6);
    assert(reply[5] == 0 && s_store.observed_clock == 1256);
    fail_nvs_commit = false;
    assert(orbit_state_handle_rpc("000000000001", request, 5 + encoded, reply, sizeof(reply)) == 6);
    assert(reply[5] == 1 && durable_clock == 3255 && s_store.observed_clock == 3000);

    request[0] = 'X';
    assert(orbit_state_handle_rpc("peer", request, 5 + encoded, reply, sizeof(reply)) == 0);
    request[0] = 'O';
    assert(orbit_state_handle_rpc("peer", request, 5 + encoded - 1, reply, sizeof(reply)) == 0);
    assert(orbit_state_handle_rpc("peer", request, 5 + encoded, reply, 5) == 0);
    assert(orbit_state_handle_rpc("peer", request, 5 + encoded, reply, 6) == 6 && reply[5] == 1);

    orbit_model_entry_t maximum = {0};
    maximum.used = true; maximum.key_len = ORBIT_MODEL_KEY_MAX;
    maximum.value_len = ORBIT_MODEL_VALUE_MAX; maximum.clock = 4000;
    maximum.author[5] = 4;
    memset(maximum.key, 'K', sizeof(maximum.key));
    memset(maximum.value, 'V', sizeof(maximum.value));
    assert(merge_entry(&maximum));

    uint8_t query[] = {'O', 'S', '0', '1', 3, 0};
    size_t n = orbit_state_handle_rpc("peer", query, sizeof(query), reply, sizeof(reply));
    assert(n == 25 && reply[4] == 3 && reply[5] == 3 && reply[6] == 0);
    assert(reply[7] == 1 && reply[8] == 2 && reply[9] == 0 && reply[10] == 'x');
    query[5] = 3;
    n = orbit_state_handle_rpc("peer", query, sizeof(query), reply, sizeof(reply));
    assert(n == 8 && reply[7] == 0); /* explicit end marker */
    query[4] = 2; query[5] = 2;
    n = orbit_state_handle_rpc("peer", query, sizeof(query), reply, 139);
    assert(n == 139 && reply[7] == 1);
    assert(orbit_state_handle_rpc("peer", query, sizeof(query), reply, 138) == 0);
    query[5] = 4;
    assert(orbit_state_handle_rpc("peer", query, sizeof(query), reply, 8) == 8 && reply[7] == 0);
    query[5] = 0;
    n = orbit_state_handle_rpc("peer", query, sizeof(query), reply, sizeof(reply));
    assert(n == 32 && reply[7] == 1);
    orbit_model_entry_t fetched;
    assert(orbit_model_decode(reply + 8, n - 8, &fetched));
    assert(fetched.key_len == 2 && fetched.key[1] == 'x');
}

static void test_tick_worker_lifecycle(void)
{
    family_state.radio = true;
    family_state.count = 1;
    family_state.members[0].reachable = true;
    memcpy(family_state.members[0].id, "000000000001", 13);
    link_running = false;
    orbit_state_tick();
    assert(!s_sync_pending && posted_fn == NULL); /* timer hook uses only link state */
    link_running = true;
    streaming = true;
    orbit_state_tick();
    assert(!s_sync_pending && posted_fn == NULL);
    streaming = false;
    remote_equal_local = true;
    omit_remote_second = false;
    rpc_count = merge_tx_count = largest_rpc_reply = task_delete_count = 0;
    run_task_inline = false;
    fail_post = true;
    orbit_state_tick();
    assert(!s_sync_pending && posted_fn == NULL);
    fail_post = false; fail_task = true;
    now_us += 31000000;
    orbit_state_tick();
    assert(s_sync_pending && posted_fn != NULL);
    posted_fn(posted_arg);
    assert(!s_sync_pending && hold_count == 0 && rpc_count == 0);
    fail_task = false; run_task_inline = true;
    now_us += 31000000;
    orbit_state_tick();
    assert(s_sync_pending && posted_fn != NULL);
    posted_fn(posted_arg);
    assert(!s_sync_pending && hold_count == 0 && rpc_count == 4);
    assert(merge_tx_count == 0 && largest_rpc_reply <= 139 && task_delete_count == 1);

    omit_remote_second = true;
    now_us += 31000000;
    orbit_state_tick();
    assert(s_sync_pending && posted_fn != NULL);
    posted_fn(posted_arg);
    assert(!s_sync_pending && hold_count == 0 && merge_tx_count == 1);
    assert(task_delete_count == 2 && largest_rpc_reply <= 139);

    link_running = false;
    now_us += 31000000;
    orbit_state_tick();
    assert(!s_sync_pending);
}

int main(void)
{
    test_init_cleanup_and_durable_reservations();
    test_rpc_validation_and_commit_failure();
    test_tick_worker_lifecycle();
    reset_service();
    puts("ORBIT state wrapper tests passed");
    return 0;
}
