#include "../link_house.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "services/ecos.h"
#include "services/ecos_orbit.h"
#include "services/notify.h"
#include "core/state.h"
#include "core/sys.h"
#include "freertos/task.h"

static ecos_family_t fake_family;
static bool fake_pmu_present = true;
static int fake_battery = 60, fake_usb;
static int fake_refresh_busy, fake_refresh_count, fake_delay_count;
static bool fake_companion;
static int hold_count, lv_lock_depth, scene_apply_count, timer_apply_count;
static int notify_count, event_count, rpc_count, task_delete_count;
static int rpc_mode; /* 0 accept, 1 explicit reject, 2 unknown timeout, 3 malformed */
static char event_type[12], event_text[16];
static char last_rpc_id[13], last_rpc[ECOS_ORBIT_REQUEST_MAX + 1];
static void (*queued_fn)(void *);
static void *queued_arg;
static bool run_task_inline, fail_task;
static int psram_attempts, psram_allocs, psram_frees, fail_alloc_on;
static int64_t fake_now_us;
static char last_timer_action[8];
static int last_timer_seconds;

void *heap_caps_calloc(size_t n, size_t size, int caps)
{
    (void)caps;
    ++psram_attempts;
    if (psram_attempts == fail_alloc_on) return NULL;
    void *p = calloc(n, size);
    if (p != NULL) ++psram_allocs;
    return p;
}
void *heap_caps_malloc(size_t size, int caps)
{
    (void)caps;
    ++psram_attempts;
    if (psram_attempts == fail_alloc_on) return NULL;
    void *p = malloc(size);
    if (p != NULL) ++psram_allocs;
    return p;
}
void heap_caps_free(void *p) { if (p != NULL) { ++psram_frees; free(p); } }

esp_err_t esp_read_mac(unsigned char *mac, int type)
{
    (void)type;
    static const uint8_t self[6] = {0, 0, 0, 0, 0, 1};
    memcpy(mac, self, 6);
    return ESP_OK;
}
int64_t esp_timer_get_time(void) { return fake_now_us; }
bool pmu_present(void) { return fake_pmu_present; }
bool pmu_usb_present(void) { return fake_usb != 0; }
int32_t state_get(state_id_t id) { return id == STATE_USB ? fake_usb : fake_battery; }
const char *settings_device_name(void) { return "Self"; }

esp_err_t ecos_family_refresh(void)
{
    ++fake_refresh_count;
    return ESP_OK;
}
void ecos_family(ecos_family_t *out)
{
    *out = fake_family;
    if (fake_refresh_busy > 0)
    {
        out->busy = true;
        --fake_refresh_busy;
    }
    else out->busy = false;
}
void ecos_family_hold(bool on) { hold_count += on ? 1 : -1; }
bool ecos_family_companion(char id[13])
{
    if (!fake_companion) return false;
    if (id != NULL) memcpy(id, "000000000099", 13);
    return true;
}

int ecos_family_orbit_large(const char *id, const uint8_t *request, size_t length,
                            uint8_t *reply, size_t max, int timeout_ms)
{
    assert(lv_lock_depth == 0);
    assert(timeout_ms > 0 && length <= ECOS_ORBIT_REQUEST_MAX && max >= 1);
    assert(length < sizeof(last_rpc));
    memcpy(last_rpc, request, length); last_rpc[length] = '\0';
    snprintf(last_rpc_id, sizeof(last_rpc_id), "%s", id);
    ++rpc_count;
    if (rpc_mode == 2) return -1;
    if (rpc_mode == 3)
    {
        if (max < 1) return -1;
        reply[0] = '{';
        return 1;
    }
    const char *answer = rpc_mode == 1 ? "{\"ok\":false}" :
                         strstr(last_rpc, "house_") != NULL ? "{\"ok\":true,\"queued\":true}" :
                         "{\"ok\":true}";
    size_t n = strlen(answer);
    assert(n <= max);
    memcpy(reply, answer, n);
    return (int)n;
}

void lv_port_lock(void) { ++lv_lock_depth; }
void lv_port_unlock(void) { assert(lv_lock_depth > 0); --lv_lock_depth; }
bool timer_remote(const char *action, int seconds)
{
    assert(lv_lock_depth == 1);
    ++timer_apply_count;
    snprintf(last_timer_action, sizeof(last_timer_action), "%s", action);
    last_timer_seconds = seconds;
    return true;
}
bool link_scene_post(cJSON *json)
{
    assert(lv_lock_depth == 0);
    ++scene_apply_count;
    assert(cJSON_IsObject(json));
    cJSON_Delete(json);
    return true;
}
void link_event_text(const char *type, const char *text)
{
    ++event_count;
    snprintf(event_type, sizeof(event_type), "%s", type);
    snprintf(event_text, sizeof(event_text), "%s", text);
}
void notify_add(const notify_t *notification)
{
    assert(lv_lock_depth == 0);
    assert(strcmp(notification->app, "House") == 0);
    ++notify_count;
}

void vTaskDelay(unsigned ticks)
{
    fake_now_us += (int64_t)ticks * 1000;
    ++fake_delay_count;
}
void vTaskDelete(void *task) { assert(task == NULL); ++task_delete_count; }
bool sys_task_create(void (*fn)(void *), const char *name, int stack,
                     void *arg, int priority, int core)
{
    assert(stack == 6144 && priority == 3 && core == SYS_CORE_ANY);
    assert(strcmp(name, "house_scene") == 0 || strcmp(name, "house_timer") == 0);
    if (fail_task) return false;
    if (run_task_inline) fn(arg);
    else { queued_fn = fn; queued_arg = arg; }
    return true;
}

#include "../link_house.c"

static cJSON *json(const char *text)
{
    cJSON *result = cJSON_Parse(text);
    assert(result != NULL);
    return result;
}

static void peer(int index, const char *id, const char *name, bool known, bool reachable,
                 uint32_t seen, int battery, bool powered)
{
    ecos_member_t *m = &fake_family.members[index];
    snprintf(m->id, sizeof(m->id), "%s", id);
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->known = known;
    m->reachable = reachable;
    m->seen_ago = seen;
    m->battery = battery;
    m->powered = powered;
    m->companion = false;
}

static void run_queued(void)
{
    assert(queued_fn != NULL);
    void (*fn)(void *) = queued_fn;
    void *arg = queued_arg;
    queued_fn = NULL; queued_arg = NULL;
    fn(arg);
}

static void reset_mocks(void)
{
    memset(&fake_family, 0, sizeof(fake_family));
    fake_pmu_present = true; fake_battery = 60; fake_usb = 0;
    fake_refresh_busy = fake_refresh_count = fake_delay_count = 0;
    fake_companion = false;
    hold_count = lv_lock_depth = scene_apply_count = timer_apply_count = 0;
    notify_count = event_count = rpc_count = task_delete_count = 0;
    rpc_mode = 0; run_task_inline = false; fail_task = false;
    psram_attempts = psram_allocs = psram_frees = fail_alloc_on = 0;
    fake_now_us = 0; queued_fn = NULL; queued_arg = NULL;
    event_type[0] = event_text[0] = last_rpc_id[0] = last_rpc[0] = last_timer_action[0] = '\0';
    last_timer_seconds = 0;
    s_job_pending = false;
}

static void test_local_head_fanout_and_refresh_wait(void)
{
    reset_mocks();
    fake_usb = 1; /* local powered wins */
    fake_family.count = 3;
    peer(0, "000000000002", "Fresh", true, true, 10, 80, false);
    peer(1, "000000000003", "Stale", true, true, 31, 100, true);
    peer(2, "000000000004", "Unknown", false, true, 1, 100, true);
    fake_refresh_busy = 2;
    assert(link_house_scene(json("{\"scene\":\"aura\",\"color\":16711680}"), false));
    assert(queued_fn != NULL && s_job_pending);
    run_queued();
    assert(fake_refresh_count == 1 && fake_delay_count == 2);
    assert(scene_apply_count == 1 && rpc_count == 1 && strcmp(last_rpc_id, "000000000002") == 0);
    assert(strstr(last_rpc, "\"op\":\"scene\"") != NULL);
    assert(strstr(last_rpc, "house_scene") == NULL);
    assert(notify_count == 1 && strcmp(event_type, "house_scene") == 0);
    assert(strcmp(event_text, "ok:2 no:0 ?:0") == 0);
    assert(hold_count == 0 && task_delete_count == 1 && !s_job_pending);
    assert(psram_allocs == psram_frees);
}

static void test_deterministic_remote_head_and_failover(void)
{
    reset_mocks();
    fake_battery = 20;
    fake_family.count = 2;
    peer(0, "000000000003", "Tie", true, true, 2, 90, false);
    peer(1, "000000000002", "Tie", true, true, 2, 90, false);
    assert(link_house_scene(json("{\"scene\":\"pulse\"}"), false));
    run_queued();
    assert(scene_apply_count == 0 && rpc_count == 1 && strcmp(last_rpc_id, "000000000002") == 0);
    assert(strstr(last_rpc, "\"op\":\"house_scene\"") != NULL);
    assert(strstr(last_rpc, "\"direct\":true") != NULL);
    assert(notify_count == 0 && strcmp(event_text, "ok:1 no:0 ?:0") == 0);

    reset_mocks();
    fake_battery = 20;
    fake_family.count = 2;
    peer(0, "000000000002", "Best", true, true, 31, 100, true);
    peer(1, "000000000003", "Next", true, true, 1, 80, false);
    assert(link_house_scene(json("{\"scene\":\"pulse\"}"), false));
    run_queued();
    assert(scene_apply_count == 0 && rpc_count == 1 && strcmp(last_rpc_id, "000000000003") == 0);
}

static void test_direct_relay_never_forwards_house_again(void)
{
    reset_mocks();
    fake_family.count = 2;
    peer(0, "000000000002", "PeerA", true, true, 1, 80, false);
    peer(1, "000000000003", "PeerB", true, true, 1, 70, false);
    assert(link_house_scene(json("{\"scene\":\"beam\",\"angle\":45}"), true));
    run_queued();
    assert(scene_apply_count == 1 && rpc_count == 2);
    assert(strstr(last_rpc, "\"op\":\"scene\"") != NULL);
    assert(strstr(last_rpc, "house_scene") == NULL);
    assert(notify_count == 1 && strcmp(event_text, "ok:3 no:0 ?:0") == 0);
}

static void test_scene_cannot_override_routing_fields(void)
{
    reset_mocks();
    assert(!link_house_scene(json("{\"scene\":\"aura\",\"direct\":false}"), false));
    assert(!link_house_scene(json("{\"scene\":\"aura\",\"op\":\"scene\"}"), false));
    assert(!s_job_pending && queued_fn == NULL && psram_allocs == psram_frees);
}

static void test_timer_validation_routing_and_ui_lock(void)
{
    reset_mocks();
    assert(!link_house_timer(json("{\"action\":\"watch\"}"), false));
    assert(!link_house_timer(json("{\"action\":\"start\",\"seconds\":0}"), false));
    assert(!link_house_timer(json("{\"action\":\"stop\",\"seconds\":1}"), false));
    assert(!link_house_timer(json("{\"action\":\"start\",\"seconds\":30,\"extra\":1}"), false));
    assert(!s_job_pending && queued_fn == NULL);

    fake_usb = 1;
    fake_family.count = 1;
    peer(0, "000000000002", "Peer", true, true, 1, 75, false);
    assert(link_house_timer(json("{\"action\":\"start\",\"seconds\":60}"), true));
    run_queued();
    assert(timer_apply_count == 1 && strcmp(last_timer_action, "start") == 0 && last_timer_seconds == 60);
    assert(lv_lock_depth == 0 && rpc_count == 1 && strstr(last_rpc, "\"op\":\"timer\"") != NULL);
    assert(strstr(last_rpc, "house_timer") == NULL && notify_count == 1);

    reset_mocks();
    fake_battery = 20;
    fake_family.count = 1;
    peer(0, "000000000002", "Powered", true, true, 1, 20, true);
    assert(link_house_timer(json("{\"action\":\"stop\"}"), false));
    run_queued();
    assert(timer_apply_count == 0 && rpc_count == 1 && strcmp(last_rpc_id, "000000000002") == 0);
    assert(strstr(last_rpc, "\"op\":\"house_timer\"") != NULL);
    assert(notify_count == 0);
}

static void test_counts_no_retry_and_cleanup(void)
{
    reset_mocks();
    fake_usb = 1;
    fake_family.count = 1;
    peer(0, "000000000002", "Peer", true, true, 1, 60, false);
    rpc_mode = 2;
    assert(link_house_scene(json("{\"scene\":\"light\"}"), true));
    run_queued();
    assert(scene_apply_count == 1 && rpc_count == 1);
    assert(strcmp(event_text, "ok:1 no:0 ?:1") == 0 && notify_count == 1);

    reset_mocks();
    fail_task = true;
    int before_allocs = psram_allocs, before_frees = psram_frees;
    assert(!link_house_scene(json("{\"scene\":\"aura\"}"), false));
    assert(!s_job_pending && queued_fn == NULL);
    assert(psram_allocs - before_allocs == psram_frees - before_frees);

    reset_mocks();
    fail_alloc_on = 2; /* PSRAM scene text allocation after the job */
    assert(!link_house_scene(json("{\"scene\":\"aura\"}"), false));
    assert(!s_job_pending && psram_allocs == psram_frees);

    reset_mocks();
    run_task_inline = true;
    fail_alloc_on = 2; /* same partial-allocation path */
    assert(!link_house_scene(json("{\"scene\":\"aura\"}"), false));
    assert(!s_job_pending && psram_allocs == psram_frees);
}

static void test_companion_delegation_before_discovery(void)
{
    reset_mocks();
    fake_companion = true;
    fake_refresh_busy = 4;
    assert(link_house_scene(json("{\"scene\":\"aura\"}"), false));
    run_queued();
    assert(rpc_count == 1 && strcmp(last_rpc_id, "000000000099") == 0);
    assert(strstr(last_rpc, "\"op\":\"house_scene\"") != NULL);
    assert(scene_apply_count == 0 && fake_refresh_count == 0 && fake_delay_count == 0);
    assert(strcmp(event_text, "ok:1 no:0 ?:0") == 0);
    assert(hold_count == 0 && psram_allocs == psram_frees);

    /* An uncertain companion response is terminal: never discover/fan out. */
    reset_mocks();
    fake_companion = true;
    rpc_mode = 2;
    assert(link_house_scene(json("{\"scene\":\"aura\"}"), false));
    run_queued();
    assert(rpc_count == 1 && fake_refresh_count == 0 && scene_apply_count == 0);
    assert(strcmp(event_text, "ok:0 no:0 ?:1") == 0);
}

static void test_proxy_peer_election(void)
{
    reset_mocks();
    fake_battery = 20;
    fake_family.count = 1;
    peer(0, "000000000002", "Phone", true, true, 1, 10, false);
    /* A companion proxy must rank ahead of every powered device. */
    fake_family.members[0].companion = true;
    assert(link_house_scene(json("{\"scene\":\"pulse\"}"), false));
    run_queued();
    assert(scene_apply_count == 0 && rpc_count == 1);
    assert(strcmp(last_rpc_id, "000000000002") == 0);
    assert(strstr(last_rpc, "\"op\":\"house_scene\"") != NULL);
}

static void test_direct_work_delegates_to_fresh_companion(void)
{
    reset_mocks();
    fake_companion = true;
    fake_family.count = 1;
    peer(0, "000000000002", "Peer", true, true, 1, 80, false);
    assert(link_house_scene(json("{\"scene\":\"beam\"}"), true));
    run_queued();
    assert(scene_apply_count == 0 && rpc_count == 1);
    assert(strcmp(last_rpc_id, "000000000099") == 0);
    assert(strstr(last_rpc, "\"op\":\"house_scene\"") != NULL);
}

int main(void)
{
    test_local_head_fanout_and_refresh_wait();
    test_deterministic_remote_head_and_failover();
    test_direct_relay_never_forwards_house_again();
    test_scene_cannot_override_routing_fields();
    test_timer_validation_routing_and_ui_lock();
    test_counts_no_retry_and_cleanup();
    test_companion_delegation_before_discovery();
    test_proxy_peer_election();
    test_direct_work_delegates_to_fresh_companion();
    puts("Orbit household tests passed");
    return 0;
}
