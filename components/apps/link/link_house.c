#include "link_house.h"

#include "link_internal.h"
#include "../../services/ecos/orbit_model.h"
#include "services/ecos.h"
#include "services/ecos_orbit.h"
#include "services/notify.h"
#include "core/lv_port.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/sys.h"
#include "hardware/pmu.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Existing app operation, safe only under the brief LVGL lock. */
extern bool timer_remote(const char *action, int seconds);

#define HOUSE_MAX_TEXT ECOS_ORBIT_REQUEST_MAX
#define HOUSE_DISCOVERY_WAIT_US (12LL * 1000000)
#define HOUSE_DISCOVERY_POLL_MS 100
#define HOUSE_RPC_TIMEOUT_MS 10000

typedef enum { HOUSE_SCENE, HOUSE_TIMER } house_kind_t;
typedef struct
{
    house_kind_t kind;
    bool direct;
    size_t scene_len;
    char *scene_text; /* PSRAM; NULL for timer jobs */
    char timer_action[6];
    int timer_seconds;
} house_job_t;

typedef struct
{
    orbit_model_node_t rank;
    char id[13];
    bool local;
    bool virtual_companion;
    int member_index;
} house_candidate_t;

static portMUX_TYPE s_job_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_job_pending;

static void house_worker(void *arg);

static bool job_claim(void)
{
    portENTER_CRITICAL(&s_job_mux);
    bool claimed = !s_job_pending;
    if (claimed) s_job_pending = true;
    portEXIT_CRITICAL(&s_job_mux);
    return claimed;
}

static void job_release(void)
{
    portENTER_CRITICAL(&s_job_mux);
    s_job_pending = false;
    portEXIT_CRITICAL(&s_job_mux);
}

static void free_job(house_job_t *job)
{
    if (job == NULL) return;
    if (job->scene_text != NULL) heap_caps_free(job->scene_text);
    heap_caps_free(job);
}

static bool parse_mac_id(const char *id, uint8_t mac[6])
{
    if (id == NULL || strlen(id) != 12) return false;
    for (size_t i = 0; i < 6; ++i)
    {
        unsigned value = 0;
        for (size_t n = 0; n < 2; ++n)
        {
            char c = id[i * 2 + n];
            unsigned nibble = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                              c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
                              c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16;
            if (nibble > 15) return false;
            value = value * 16 + nibble;
        }
        mac[i] = (uint8_t)value;
    }
    return orbit_model_author_valid(mac);
}

static void mac_id(const uint8_t mac[6], char id[13])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 6; ++i)
    {
        id[i * 2] = hex[mac[i] >> 4];
        id[i * 2 + 1] = hex[mac[i] & 15];
    }
    id[12] = '\0';
}

static uint8_t bounded_name_copy(uint8_t *out, const char *name)
{
    size_t length = 0;
    if (name != NULL)
        while (length < ORBIT_MODEL_KEY_MAX && name[length] != '\0') ++length;
    if (length != 0) memcpy(out, name, length);
    return (uint8_t)length;
}

static bool family_candidate(const ecos_member_t *member)
{
    return member->known && member->reachable && member->seen_ago <= 30;
}

static size_t build_candidates(const ecos_family_t *family, house_candidate_t *items,
                               uint8_t self_mac[6])
{
    if (esp_read_mac(self_mac, ESP_MAC_WIFI_STA) != ESP_OK || !orbit_model_author_valid(self_mac)) return 0;
    size_t count = 0;
    house_candidate_t *local = &items[count++];
    memset(local, 0, sizeof(*local));
    local->rank.awake = true;
    /* Companion ranking remains false until the phone is represented as a
     * first-class reachable household node. */
    local->rank.companion = ecos_family_companion(NULL);
    local->rank.powered = !pmu_present() || state_get(STATE_USB) != 0;
    int battery = pmu_present() ? (int)state_get(STATE_BATTERY) : -1;
    local->rank.battery_known = battery >= 0 && battery <= 100;
    local->rank.battery = local->rank.battery_known ? (uint8_t)battery : 0;
    local->rank.name_len = bounded_name_copy(local->rank.name, settings_device_name());
    memcpy(local->rank.mac, self_mac, 6);
    local->local = true;
    local->member_index = -1;
    mac_id(self_mac, local->id);

    int peers = family->count;
    if (peers < 0) peers = 0;
    if (peers > ECOS_PEERS_MAX) peers = ECOS_PEERS_MAX;
    for (int i = 0; i < peers && count < ECOS_PEERS_MAX + 1; ++i)
    {
        const ecos_member_t *member = &family->members[i];
        if (!family_candidate(member)) continue;
        bool virtual_companion = strcmp(member->product, "Orbit Companion") == 0;
        if (virtual_companion && !member->companion) continue;
        uint8_t mac[6];
        if (!parse_mac_id(member->id, mac) || memcmp(mac, self_mac, 6) == 0) continue;
        house_candidate_t *candidate = &items[count++];
        memset(candidate, 0, sizeof(*candidate));
        candidate->rank.awake = true;
        candidate->rank.companion = member->companion;
        candidate->rank.powered = member->powered || member->usb || member->charging;
        candidate->rank.battery_known = member->battery >= 0 && member->battery <= 100;
        candidate->rank.battery = candidate->rank.battery_known ? (uint8_t)member->battery : 0;
        candidate->rank.name_len = bounded_name_copy(candidate->rank.name, member->name);
        memcpy(candidate->rank.mac, mac, 6);
        memcpy(candidate->id, member->id, sizeof(candidate->id));
        candidate->member_index = i;
        candidate->virtual_companion = virtual_companion;
    }
    return count;
}

static int find_candidate(const house_candidate_t *items, size_t count, const uint8_t mac[6])
{
    for (size_t i = 0; i < count; ++i)
        if (memcmp(items[i].rank.mac, mac, 6) == 0) return (int)i;
    return -1;
}

static bool refresh_family(ecos_family_t *family)
{
    esp_err_t refresh = ecos_family_refresh();
    if (refresh != ESP_OK)
    {
        ecos_family(family);
        if (!family->busy) return false;
    }
    int64_t until = esp_timer_get_time() + HOUSE_DISCOVERY_WAIT_US;
    do
    {
        ecos_family(family);
        if (!family->busy) return true;
        vTaskDelay(pdMS_TO_TICKS(HOUSE_DISCOVERY_POLL_MS));
    } while (esp_timer_get_time() < until);
    ecos_family(family);
    return !family->busy;
}

static bool scene_valid(const cJSON *json)
{
    if (!cJSON_IsObject(json)) return false;
    for (const cJSON *item = json->child; item != NULL; item = item->next)
        if (item->string == NULL || strcmp(item->string, "op") == 0 ||
            strcmp(item->string, "direct") == 0) return false;
    const char *scene = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "scene"));
    static const char *const names[] = {"clear", "pulse", "aura", "beam", "text", "light", "orbi", "dial"};
    for (size_t i = 0; scene != NULL && i < sizeof(names) / sizeof(names[0]); ++i)
        if (strcmp(scene, names[i]) == 0) return true;
    return false;
}

static char *print_scene(cJSON *json, size_t *length)
{
    char *printed = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (printed == NULL) return NULL;
    size_t n = strlen(printed);
    size_t prefix = sizeof("{\"op\":\"house_scene\",\"direct\":true") - 1;
    if (n < 2 || n > HOUSE_MAX_TEXT - prefix)
    {
        cJSON_free(printed);
        return NULL;
    }
    char *copy = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy != NULL) memcpy(copy, printed, n + 1);
    cJSON_free(printed);
    if (copy != NULL) *length = n;
    return copy;
}

bool link_house_scene(cJSON *json, bool direct)
{
    if (json == NULL) return false;
    if (!scene_valid(json) || !job_claim())
    {
        cJSON_Delete(json);
        return false;
    }
    house_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job == NULL)
    {
        cJSON_Delete(json);
        job_release();
        return false;
    }
    job->kind = HOUSE_SCENE;
    job->direct = direct;
    job->scene_text = print_scene(json, &job->scene_len);
    if (job->scene_text == NULL)
    {
        free_job(job);
        job_release();
        return false;
    }
    if (!sys_task_create(house_worker, "house_scene", 6144, job, 3, SYS_CORE_ANY))
    {
        free_job(job);
        job_release();
        return false;
    }
    return true;
}

static bool timer_args(cJSON *json, char action[6], int *seconds)
{
    if (!cJSON_IsObject(json)) return false;
    const cJSON *action_item = cJSON_GetObjectItemCaseSensitive(json, "action");
    const char *name = cJSON_GetStringValue(action_item);
    const cJSON *seconds_item = cJSON_GetObjectItemCaseSensitive(json, "seconds");
    if (name == NULL || (strcmp(name, "start") != 0 && strcmp(name, "stop") != 0)) return false;
    int fields = 0;
    for (const cJSON *item = json->child; item != NULL; item = item->next)
    {
        ++fields;
        if (item != action_item && item != seconds_item) return false;
    }
    if (fields != (seconds_item != NULL ? 2 : 1)) return false;
    if (strcmp(name, "start") == 0)
    {
        if (!cJSON_IsNumber(seconds_item) || seconds_item->valuedouble < 1 ||
            seconds_item->valuedouble > 24 * 3600 ||
            seconds_item->valuedouble != (double)(int)seconds_item->valuedouble) return false;
        *seconds = (int)seconds_item->valuedouble;
    }
    else
    {
        if (seconds_item != NULL) return false;
        *seconds = 0;
    }
    memcpy(action, name, strlen(name) + 1);
    return true;
}

bool link_house_timer(cJSON *json, bool direct)
{
    if (json == NULL) return false;
    char action[6] = {0};
    int seconds = 0;
    if (!timer_args(json, action, &seconds) || !job_claim())
    {
        cJSON_Delete(json);
        return false;
    }
    cJSON_Delete(json);
    house_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job == NULL)
    {
        job_release();
        return false;
    }
    job->kind = HOUSE_TIMER;
    job->direct = direct;
    memcpy(job->timer_action, action, sizeof(action));
    job->timer_seconds = seconds;
    if (!sys_task_create(house_worker, "house_timer", 6144, job, 3, SYS_CORE_ANY))
    {
        free_job(job);
        job_release();
        return false;
    }
    return true;
}

static size_t make_scene_rpc(const house_job_t *job, bool relay, uint8_t *out, size_t capacity)
{
    const char *prefix = relay ? "{\"op\":\"house_scene\",\"direct\":true" : "{\"op\":\"scene\"";
    size_t prefix_len = strlen(prefix);
    if (job->scene_len < 2 || job->scene_text[0] != '{' || job->scene_text[job->scene_len - 1] != '}' ||
        capacity < prefix_len + (job->scene_len > 2 ? job->scene_len : 1)) return 0;
    size_t n = 0;
    memcpy(out + n, prefix, prefix_len); n += prefix_len;
    if (job->scene_len > 2)
    {
        out[n++] = ',';
        memcpy(out + n, job->scene_text + 1, job->scene_len - 2);
        n += job->scene_len - 2;
    }
    out[n++] = '}';
    return n;
}

static size_t make_timer_rpc(const house_job_t *job, bool relay, uint8_t *out, size_t capacity)
{
    int n = job->kind == HOUSE_TIMER && strcmp(job->timer_action, "start") == 0
                ? snprintf((char *)out, capacity, relay
                           ? "{\"op\":\"house_timer\",\"direct\":true,\"action\":\"start\",\"seconds\":%d}"
                           : "{\"op\":\"timer\",\"action\":\"start\",\"seconds\":%d}", job->timer_seconds)
                : snprintf((char *)out, capacity, relay
                           ? "{\"op\":\"house_timer\",\"direct\":true,\"action\":\"stop\"}"
                           : "{\"op\":\"timer\",\"action\":\"stop\"}");
    return n > 0 && (size_t)n < capacity ? (size_t)n : 0;
}

typedef enum { OUTCOME_OK, OUTCOME_NO, OUTCOME_UNKNOWN } outcome_t;

static outcome_t rpc_outcome(const char *id, const uint8_t *request, size_t request_len, bool queued)
{
    uint8_t reply[ECOS_ORBIT_MAX];
    int n = ecos_family_orbit_large(id, request, request_len, reply, sizeof(reply), HOUSE_RPC_TIMEOUT_MS);
    if (n <= 0) return OUTCOME_UNKNOWN;
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts((const char *)reply, (size_t)n, &end, false);
    if (!cJSON_IsObject(json) || end != (const char *)reply + n)
    {
        cJSON_Delete(json);
        return OUTCOME_UNKNOWN;
    }
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(json, "ok");
    const cJSON *is_queued = cJSON_GetObjectItemCaseSensitive(json, "queued");
    outcome_t result = cJSON_IsTrue(ok) && (!queued || cJSON_IsTrue(is_queued)) ? OUTCOME_OK :
                       cJSON_IsFalse(ok) ? OUTCOME_NO : OUTCOME_UNKNOWN;
    cJSON_Delete(json);
    return result;
}

static void add_outcome(outcome_t outcome, unsigned *ok, unsigned *no, unsigned *unknown)
{
    if (outcome == OUTCOME_OK) ++*ok;
    else if (outcome == OUTCOME_NO) ++*no;
    else ++*unknown;
}

static bool apply_scene_local(const house_job_t *job)
{
    cJSON *scene = cJSON_ParseWithLengthOpts(job->scene_text, job->scene_len + 1, NULL, true);
    return scene != NULL && link_scene_post(scene); /* takes ownership */
}

static bool apply_timer_local(const house_job_t *job)
{
    lv_port_lock();
    bool ok = timer_remote(job->timer_action, job->timer_seconds);
    lv_port_unlock();
    return ok;
}

static void publish_result(const house_job_t *job, unsigned ok, unsigned no, unsigned unknown, bool at_head)
{
    char result[16];
    snprintf(result, sizeof(result), "ok:%u no:%u ?:%u", ok, no, unknown);
    link_event_text(job->kind == HOUSE_SCENE ? "house_scene" : "house_timer", result);
    if (!at_head) return;
    notify_t notification = {0};
    notification.time = time(NULL);
    snprintf(notification.app, sizeof(notification.app), "House");
    snprintf(notification.title, sizeof(notification.title), "%s", job->kind == HOUSE_SCENE ? "Scene" : "Timer");
    snprintf(notification.body, sizeof(notification.body), "%s", result);
    notify_add(&notification);
}

static void house_worker(void *arg)
{
    house_job_t *job = arg;
    unsigned ok = 0, no = 0, unknown = 0;
    bool held = false;
    bool at_head = job->direct;
    ecos_family_t *family = heap_caps_calloc(1, sizeof(*family), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t self_mac[6] = {0};
    house_candidate_t candidates[ECOS_PEERS_MAX + 1] = {0};
    uint8_t *request = heap_caps_malloc(HOUSE_MAX_TEXT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (family == NULL || request == NULL)
    {
        ++no;
        goto publish;
    }
    ecos_family_hold(true);
    held = true;
    /* The gateway advertises the authenticated companion's presence to its
     * paired neighbours. Delegate before discovery: the companion fans out
     * local scene/timer operations, never another house election. */
    char companion[13];
    if (ecos_family_companion(companion))
    {
        size_t request_len = job->kind == HOUSE_SCENE
                                 ? make_scene_rpc(job, true, request, HOUSE_MAX_TEXT)
                                 : make_timer_rpc(job, true, request, HOUSE_MAX_TEXT);
        add_outcome(request_len != 0
                        ? rpc_outcome(companion, request, request_len, true)
                        : OUTCOME_NO,
                    &ok, &no, &unknown);
        goto publish;
    }
    if (!refresh_family(family))
    {
        ++unknown;
        goto publish;
    }
    size_t candidate_count = build_candidates(family, candidates, self_mac);
    if (candidate_count == 0)
    {
        ++no;
        goto publish;
    }

    if (job->direct)
    {
        bool applied = job->kind == HOUSE_SCENE ? apply_scene_local(job) : apply_timer_local(job);
        add_outcome(applied ? OUTCOME_OK : OUTCOME_NO, &ok, &no, &unknown);
    }
    else
    {
        uint8_t coordinator_mac[6];
        bool found = false;
        orbit_model_node_t rank[ECOS_PEERS_MAX + 1];
        for (size_t i = 0; i < candidate_count; ++i) rank[i] = candidates[i].rank;
        found = orbit_model_coordinator(rank, candidate_count, coordinator_mac);
        int head = found ? find_candidate(candidates, candidate_count, coordinator_mac) : -1;
        if (head < 0)
        {
            ++no;
            goto publish;
        }
        if (!candidates[head].local)
        {
            size_t request_len = job->kind == HOUSE_SCENE
                                     ? make_scene_rpc(job, true, request, HOUSE_MAX_TEXT)
                                     : make_timer_rpc(job, true, request, HOUSE_MAX_TEXT);
            add_outcome(request_len != 0
                            ? rpc_outcome(candidates[head].id, request, request_len, true)
                            : OUTCOME_NO,
                        &ok, &no, &unknown);
            goto publish;
        }
        at_head = true;

        bool applied = job->kind == HOUSE_SCENE ? apply_scene_local(job) : apply_timer_local(job);
        add_outcome(applied ? OUTCOME_OK : OUTCOME_NO, &ok, &no, &unknown);
    }

    /* Direct work and a locally elected coordinator both execute here. */
    if (ok > 0)
    {
        for (size_t i = 0; i < candidate_count; ++i)
        {
            if (candidates[i].local || candidates[i].virtual_companion) continue;
            size_t request_len = job->kind == HOUSE_SCENE
                                     ? make_scene_rpc(job, false, request, HOUSE_MAX_TEXT)
                                     : make_timer_rpc(job, false, request, HOUSE_MAX_TEXT);
            outcome_t outcome = request_len != 0
                                    ? rpc_outcome(candidates[i].id, request, request_len, false)
                                    : OUTCOME_NO;
            add_outcome(outcome, &ok, &no, &unknown);
        }
    }

publish:
    publish_result(job, ok, no, unknown, at_head);
    if (held) ecos_family_hold(false);
    if (request != NULL) heap_caps_free(request);
    if (family != NULL) heap_caps_free(family);
    free_job(job);
    job_release();
    vTaskDelete(NULL);
}
