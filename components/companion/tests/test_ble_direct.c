#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#define REPLY_MAX 98304
#define IDLE_EXIT_US 8000000LL
#define BLE_HS_CONN_HANDLE_NONE UINT16_MAX
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef bool (*ble_api_handler_t)(const char *, const char *, uint8_t **, size_t *, int *);
typedef struct { uint32_t id; uint16_t conn; char path[201]; char *body; } request_t;
static _Atomic(ble_api_handler_t) s_api_handler;
static atomic_bool s_worker;
static int s_queue;
static int replies, errors, frees, handler_mode, received, receive_step, scenario, stops, deleted;
static int64_t now_us;
static request_t *arrived;
static void tracked_free(void *p) { if (p) { ++frees; free(p); } }
static bool handler(const char *path, const char *body, uint8_t **reply, size_t *length, int *status)
{
    assert(strcmp(path, "/api/link/family") == 0 && body == NULL);
    if (handler_mode == 0) return false;
    *reply = malloc(3); memcpy(*reply, "{}", 3); *length = 2; *status = 200;
    if (handler_mode == 2) *length = REPLY_MAX + 1;
    if (handler_mode == 3) *status = 999;
    if (handler_mode == 4) return false;
    return true;
}
static void reply_error(uint32_t id, int status, const char *message)
{ assert(id == 7 && status == 503 && message); ++errors; }
static void send_response(uint32_t id, int status, const uint8_t *reply, int length)
{ assert(id == 7 && status == 200 && length == 2 && memcmp(reply, "{}", 2) == 0); ++replies; }
static int64_t esp_timer_get_time(void) { return now_us; }
static void link_speed(uint16_t conn, bool fast) { (void)conn; (void)fast; }
static void web_server_stop(void) { ++stops; }
static void vTaskDelete(void *task) { assert(task == NULL); ++deleted; }
static void run(request_t *r, bool *local, int64_t *last_local)
{
    assert(r && atomic_load(&s_worker)); ++received;
    if (scenario == 1 && received == 1) { *local = true; *last_local = now_us; }
    if (scenario == 1 && received == 2) now_us += IDLE_EXIT_US;
}
static request_t *request(void) { request_t *r = calloc(1, sizeof(*r)); r->conn = 3; return r; }
static int xQueueReceive(int queue, request_t **out, int ticks)
{
    (void)queue; (void)ticks; ++receive_step;
    if (scenario == 1 && receive_step <= 2) { *out = request(); return pdTRUE; }
    if (scenario == 2 && arrived != NULL) { *out = arrived; arrived = NULL; return pdTRUE; }
    return 0;
}
static unsigned uxQueueMessagesWaiting(int queue)
{
    (void)queue;
    assert(!atomic_load(&s_worker));
    if (scenario == 2 && receive_step == 1) { arrived = request(); return 1; }
    if (scenario == 3) { atomic_store(&s_worker, true); return 1; }
    return 0;
}
#define free tracked_free
/* PRODUCTION */
#undef free

int main(void)
{
    request_t r = {.id = 7}; strcpy(r.path, "/api/link/family");
    assert(!run_direct(&r));
    ble_companion_set_api_handler(handler);
    assert(!run_direct(&r));
    handler_mode = 1;
    assert(run_direct(&r) && replies == 1 && frees == 1);
    handler_mode = 2;
    assert(run_direct(&r) && errors == 1 && frees == 2);
    handler_mode = 3;
    assert(run_direct(&r) && errors == 2 && frees == 3);
    handler_mode = 4;
    assert(!run_direct(&r) && frees == 4);
    ble_companion_set_api_handler(NULL);
    assert(!run_direct(&r));
    for (scenario = 1; scenario <= 3; ++scenario)
    {
        received = receive_step = stops = deleted = 0; now_us = 0;
        atomic_store(&s_worker, true);
        worker(NULL);
        assert(deleted == 1);
        if (scenario == 1) assert(received == 2 && stops == 1 && !atomic_load(&s_worker));
        if (scenario == 2) assert(received == 1 && stops == 0 && !atomic_load(&s_worker));
        if (scenario == 3) assert(received == 0 && atomic_load(&s_worker));
    }
    puts("BLE direct dispatch, bounded replies and worker handoff tests passed");
    return 0;
}
