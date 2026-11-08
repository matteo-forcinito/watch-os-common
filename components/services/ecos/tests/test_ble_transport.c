#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "../ecos_link_wire.h"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_MAC_WIFI_STA 0
#define ECOS_PEERS_MAX 6
#define ECOS_NAME_SIZE 24
#define ECOS_RECORD_SIZE (6 + ECOS_NAME_SIZE + 32)
#define ECOS_STORE_SIZE (2 + ECOS_PEERS_MAX * ECOS_RECORD_SIZE)
#define pdTRUE 1
typedef int portMUX_TYPE;
typedef struct queue_s { int id; } *QueueHandle_t;
typedef struct
{
    uint8_t mac[6];
    uint8_t length;
    int8_t rssi;
    uint8_t data[ECOS_LINK_FRAME_MAX];
} link_frame_t;
typedef bool (*ecos_family_ble_send_t)(const uint8_t *, size_t, void *);
bool ecos_family_ble_open(ecos_family_ble_send_t send, void *context, const uint8_t mac[6]);
bool ecos_family_ble_receive(const uint8_t *frame, size_t length, void *context);
void ecos_family_ble_close(void *context);
bool ecos_family_companion(char id[13]);
typedef struct
{
    uint8_t mac[6];
    uint8_t local_mac[6];
    ecos_family_ble_send_t send;
    void *context;
    unsigned users;
    bool active, closing, hold;
} ble_link_t;
typedef struct
{
    uint8_t mac[6];
    bool used, known, reachable;
    int64_t status_seen_at;
    ecos_link_status_t status;
} member_info_t;
static portMUX_TYPE s_lock;
static QueueHandle_t s_rx;
static unsigned s_rx_users;
static ble_link_t s_ble_link;
static member_info_t s_info[ECOS_PEERS_MAX + 2];
static int64_t fake_now;
static bool s_running, s_suspend;
static int s_holds, fail_store, fail_task_start, bus_sends, callback_sends, queue_sends, terminal_sends;
static uint8_t s_store[ECOS_STORE_SIZE], local_mac[6] = {8,7,6,5,4,3};
static int critical_depth;
static bool close_during_start;
static HANDLE send_entered, send_release, queue_entered, queue_release, close_done, unpublish_done;
static bool block_send, block_queue;
static int last_queue_result = pdTRUE;
typedef struct { void *context; bool result; const uint8_t *peer; } open_arg_t;

#define portENTER_CRITICAL(m) do { (void)(m); critical_depth++; } while (0)
#define portEXIT_CRITICAL(m) do { (void)(m); assert(critical_depth == 1); critical_depth--; } while (0)
static bool ecos_store_load(uint8_t out[ECOS_STORE_SIZE])
{
    assert(critical_depth == 0);
    if (fail_store) return false;
    memcpy(out, s_store, sizeof(s_store));
    return true;
}
static void mbedtls_platform_zeroize(void *p, size_t n) { memset(p, 0, n); }
static esp_err_t esp_read_mac(uint8_t out[6], int mode)
{
    assert(critical_depth == 0 && mode == ESP_MAC_WIFI_STA);
    memcpy(out, local_mac, 6);
    return ESP_OK;
}
static bool family_disabled(void) { assert(critical_depth == 0); return false; }
static int64_t esp_timer_get_time(void) { assert(critical_depth == 0); return fake_now; }
static void ecos_family_hold(bool on)
{
    assert(critical_depth == 0);
    s_holds += on ? 1 : (s_holds > 0 ? -1 : 0);
}
static void start_link(void)
{
    assert(critical_depth == 0);
    if (!fail_task_start)
    {
        s_running = true;
    }
    if (close_during_start && s_ble_link.context != NULL)
    {
        ecos_family_ble_close(s_ble_link.context);
    }
}
static bool xQueueSend(QueueHandle_t queue, const void *frame, int wait)
{
    assert(critical_depth == 0 && queue != NULL && wait == 0 && frame != NULL);
    ++queue_sends;
    if (block_queue)
    {
        SetEvent(queue_entered);
        WaitForSingleObject(queue_release, INFINITE);
    }
    return last_queue_result == pdTRUE;
}
static void vTaskDelay(unsigned ticks) { (void)ticks; Sleep(1); }
static esp_err_t ecos_bus_send(const uint8_t *mac, const uint8_t *frame, size_t length)
{
    assert(critical_depth == 0 && mac != NULL && frame != NULL && length > 0);
    ++bus_sends;
    return ESP_OK;
}

static bool send_callback(const uint8_t *frame, size_t length, void *context)
{
    assert(critical_depth == 0 && context != NULL);
    if (frame == NULL && length == 0)
    {
        ++terminal_sends;
        return true;
    }
    assert(frame != NULL && length > 0);
    ++callback_sends;
    if (block_send)
    {
        SetEvent(send_entered);
        WaitForSingleObject(send_release, INFINITE);
    }
    return true;
}

static bool callback_fail(const uint8_t *frame, size_t length, void *context)
{
    (void)context;
    assert(critical_depth == 0);
    ++callback_sends;
    if (frame == NULL && length == 0) ++terminal_sends;
    return false;
}

/* PRODUCTION */

static DWORD WINAPI send_thread(void *arg)
{
    uint8_t frame[4] = {'E','L','0','2'};
    (void)arg;
    send_frame(s_ble_link.mac, frame, sizeof(frame));
    return 0;
}
static DWORD WINAPI close_thread(void *arg)
{
    ecos_family_ble_close(arg);
    SetEvent(close_done);
    return 0;
}
static DWORD WINAPI queue_thread(void *arg)
{
    uint8_t frame[4] = {'E','L','0','2'};
    (void)arg;
    assert(queue_frame(s_ble_link.mac, 0, frame, sizeof(frame)));
    return 0;
}
static DWORD WINAPI unpublish_thread(void *arg)
{
    (void)arg;
    assert(unpublish_rx() != NULL);
    SetEvent(unpublish_done);
    return 0;
}

static DWORD WINAPI open_thread(void *arg)
{
    open_arg_t *open = (open_arg_t *)arg;
    open->result = ecos_family_ble_open(send_callback, open->context, open->peer);
    return 0;
}

static void setup_store(const uint8_t mac[6])
{
    memset(s_store, 0, sizeof(s_store));
    s_store[0] = 1;
    s_store[1] = 1;
    memcpy(s_store + 2, mac, 6);
}

static void setup_event(HANDLE *event)
{
    *event = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(*event != NULL);
}

static void test_open_validation_and_send_routing(void)
{
    const uint8_t peer[6] = {1,2,3,4,5,6};
    const uint8_t other[6] = {6,5,4,3,2,1};
    uint8_t frame[4] = {'E','L','0','2'};
    int context_a, context_b;
    setup_store(peer);
    ecos_family_ble_close(&context_b);
    assert(s_holds == 0 && !s_ble_link.active);
    fail_store = 1;
    assert(!ecos_family_ble_open(send_callback, &context_a, peer));
    fail_store = 0;
    assert(!ecos_family_ble_open(send_callback, &context_a, other));
    fail_task_start = 1;
    assert(!ecos_family_ble_open(send_callback, &context_a, peer));
    fail_task_start = 0;
    assert(s_holds == 0 && !s_ble_link.active);
    assert(ecos_family_ble_open(send_callback, &context_a, peer));
    assert(s_holds == 1 && s_ble_link.active);
    assert(!ecos_family_ble_open(send_callback, &context_b, peer));
    assert(send_frame(peer, frame, sizeof(frame)) == 1 && callback_sends == 1 && bus_sends == 0);
    assert(send_frame(other, frame, sizeof(frame)) == 0 && bus_sends == 1);
    portENTER_CRITICAL(&s_lock);
    s_ble_link.send = callback_fail;
    portEXIT_CRITICAL(&s_lock);
    assert(send_frame(peer, frame, sizeof(frame)) == -1 && bus_sends == 1);
    portENTER_CRITICAL(&s_lock);
    s_ble_link.send = send_callback;
    portEXIT_CRITICAL(&s_lock);
    assert(ble_send_bound(peer, NULL, 0) == 1 && terminal_sends == 1);
    ecos_family_ble_close(&context_a);
    assert(s_holds == 0 && !s_ble_link.active && !s_ble_link.closing);
}

static void test_open_close_during_startup(void)
{
    const uint8_t peer[6] = {1,2,3,4,5,6};
    int context;
    setup_store(peer);
    close_during_start = true;
    assert(!ecos_family_ble_open(send_callback, &context, peer));
    close_during_start = false;
    assert(s_holds == 0 && !s_ble_link.active && !s_ble_link.closing);
}

static void test_concurrent_open_reservation(void)
{
    const uint8_t peer[6] = {1,2,3,4,5,6};
    int context_a, context_b;
    setup_store(peer);
    open_arg_t a = {.context = &context_a, .peer = peer};
    open_arg_t b = {.context = &context_b, .peer = peer};
    HANDLE first = CreateThread(NULL, 0, open_thread, &a, 0, NULL);
    HANDLE second = CreateThread(NULL, 0, open_thread, &b, 0, NULL);
    assert(first != NULL && second != NULL);
    assert(WaitForSingleObject(first, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(second, 5000) == WAIT_OBJECT_0);
    CloseHandle(first);
    CloseHandle(second);
    assert(a.result != b.result);
    assert(s_holds == 1 && s_ble_link.active);
    ecos_family_ble_close(a.result ? &context_a : &context_b);
    assert(s_holds == 0 && !s_ble_link.active && !s_ble_link.closing);
}

static void test_companion_freshness_and_close(void)
{
    const uint8_t peer[6] = {1,2,3,4,5,6};
    const uint8_t other[6] = {6,5,4,3,2,1};
    int context;
    char id[13];
    memset(s_info, 0, sizeof(s_info));
    setup_store(peer);
    fake_now = 100000000;
    assert(ecos_family_ble_open(send_callback, &context, peer));
    assert(!ecos_family_companion(NULL)); /* no authenticated EL02 status */

    s_info[0].used = s_info[0].known = s_info[0].reachable = true;
    memcpy(s_info[0].mac, other, 6);
    s_info[0].status.flags = ECOS_LINK_F_COMPANION;
    s_info[0].status_seen_at = fake_now;
    assert(!ecos_family_companion(NULL)); /* status belongs to another peer */

    memcpy(s_info[0].mac, peer, 6);
    s_info[0].known = false;
    assert(!ecos_family_companion(NULL));
    s_info[0].known = true;
    s_info[0].status.flags = 0;
    assert(!ecos_family_companion(NULL));
    s_info[0].status.flags = ECOS_LINK_F_COMPANION;
    s_info[0].reachable = false;
    assert(!ecos_family_companion(NULL));
    s_info[0].reachable = true;
    assert(ecos_family_companion(id));
    assert(strcmp(id, "010203040506") == 0);
    assert(ecos_family_companion(NULL));
    fake_now += 30000000;
    assert(ecos_family_companion(NULL)); /* 30 seconds is still fresh */
    fake_now += 1;
    assert(!ecos_family_companion(NULL)); /* strictly fresher than 30 seconds */

    fake_now = s_info[0].status_seen_at + 1;
    assert(ecos_family_companion(NULL));
    assert(!ble_termination_required(true, false, false, false));
    assert(ble_termination_required(true, false, false, true)); /* credentials reload */
    int terminals_before = terminal_sends;
    assert(ble_send_bound(peer, NULL, 0) == 1 && terminal_sends == terminals_before + 1);
    ecos_family_ble_close(&context);
    assert(!ecos_family_companion(NULL));
    assert(s_info[0].known && !s_info[0].reachable); /* keep metadata, invalidate availability */
}

static void test_companion_wire_flag(void)
{
    uint8_t payload[ECOS_LINK_STATUS_SIZE];
    ecos_link_status_t status = {.name = "Gateway", .battery = 50, .flags = ECOS_LINK_F_COMPANION};
    ecos_link_status_t decoded;
    ecos_link_put_status(payload, &status);
    ecos_link_get_status(payload, &decoded);
    assert(decoded.flags == ECOS_LINK_F_COMPANION);
}

static void test_receive_validation_and_lifetime(void)
{
    const uint8_t peer[6] = {1,2,3,4,5,6};
    int context_a, context_b;
    setup_store(peer);
    assert(ecos_family_ble_open(send_callback, &context_a, peer));
    struct queue_s queue = {.id = 1};
    s_rx = &queue;
    uint8_t frame[ECOS_LINK_HEADER + ECOS_LINK_TIME_SIZE + ECOS_LINK_TAG] = {0};
    ecos_link_header(frame, ECOS_LINK_STATUS, peer, local_mac, 1, 1);
    assert(ecos_family_ble_receive(frame, sizeof(frame), &context_a));
    assert(queue_sends == 1);
    assert(!ecos_family_ble_receive(frame, sizeof(frame), &context_b));
    assert(!ecos_family_ble_receive(frame, sizeof(frame)-1, &context_a));
    uint8_t oversized[ECOS_LINK_FRAME_MAX + 1] = {0};
    ecos_link_header(oversized, ECOS_LINK_STATUS, peer, local_mac, 1, 1);
    assert(!ecos_family_ble_receive(oversized, sizeof(oversized), &context_a));
    const uint8_t bad_sender[6] = {9,9,9,9,9,9};
    memcpy(frame + 5, bad_sender, 6);
    assert(!ecos_family_ble_receive(frame, sizeof(frame), &context_a));
    memcpy(frame + 5, peer, 6);
    memcpy(frame + 11, bad_sender, 6);
    assert(!ecos_family_ble_receive(frame, sizeof(frame), &context_a));
    assert(queue_sends == 1);
    memcpy(frame + 11, local_mac, 6);

    setup_event(&send_entered); setup_event(&send_release); setup_event(&close_done);
    block_send = true;
    HANDLE tx = CreateThread(NULL, 0, send_thread, NULL, 0, NULL);
    assert(tx != NULL && WaitForSingleObject(send_entered, 5000) == WAIT_OBJECT_0);
    HANDLE closer = CreateThread(NULL, 0, close_thread, &context_a, 0, NULL);
    assert(closer != NULL);
    Sleep(20);
    assert(WaitForSingleObject(close_done, 0) == WAIT_TIMEOUT);
    assert(s_ble_link.closing && !s_ble_link.active && s_ble_link.users == 1);
    SetEvent(send_release);
    assert(WaitForSingleObject(tx, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(closer, 5000) == WAIT_OBJECT_0);
    CloseHandle(tx); CloseHandle(closer);
    CloseHandle(send_entered); CloseHandle(send_release); CloseHandle(close_done);
    block_send = false;
    assert(s_holds == 0 && !s_ble_link.closing && s_ble_link.context == NULL);

    int context_c;
    assert(ecos_family_ble_open(send_callback, &context_c, peer));
    ecos_family_ble_close(&context_a); /* stale close cannot unpublish the replacement */
    assert(s_ble_link.active && s_ble_link.context == &context_c && s_holds == 1);
    ecos_family_ble_close(&context_c);

    setup_event(&queue_entered); setup_event(&queue_release); setup_event(&unpublish_done);
    s_rx = &queue;
    block_queue = true;
    HANDLE rx = CreateThread(NULL, 0, queue_thread, NULL, 0, NULL);
    assert(rx != NULL && WaitForSingleObject(queue_entered, 5000) == WAIT_OBJECT_0);
    HANDLE unpub = CreateThread(NULL, 0, unpublish_thread, NULL, 0, NULL);
    assert(unpub != NULL);
    Sleep(20);
    assert(s_rx == NULL && s_rx_users == 1);
    assert(WaitForSingleObject(unpublish_done, 0) == WAIT_TIMEOUT);
    SetEvent(queue_release);
    assert(WaitForSingleObject(rx, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(unpub, 5000) == WAIT_OBJECT_0);
    CloseHandle(rx); CloseHandle(unpub);
    CloseHandle(queue_entered); CloseHandle(queue_release); CloseHandle(unpublish_done);
    block_queue = false;
    assert(s_rx_users == 0);
}

int main(void)
{
    uint8_t peer[6] = {1,2,3,4,5,6};
    setup_store(peer);
    test_open_validation_and_send_routing();
    test_open_close_during_startup();
    test_concurrent_open_reservation();
    test_companion_freshness_and_close();
    test_companion_wire_flag();
    test_receive_validation_and_lifetime();
    puts("PASS: EL02 BLE bridge production routing, RX validation and close drain");
    return 0;
}
