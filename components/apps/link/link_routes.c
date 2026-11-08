#include "link_routes.h"

#include "link_internal.h"
#include "link_orbit_json.h"
#include "core/sys.h"
#include "services/ecos.h"
#include "services/orbit_state.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define ROUTE_COUNT 8
#define ROUTE_ID_LEN 13
#define ROUTE_TIMEOUT_MS 10000
typedef struct route_job
{
    struct route_job *next;
    char type[12];
    char target[ROUTE_ID_LEN];
    char text[16];
    int value;
} route_job_t;
static char s_self[ROUTE_ID_LEN];
static route_job_t *s_head, *s_tail;
static unsigned s_count;
static bool s_worker_posted;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool valid_id(const char *id)
{
    if (!id || strlen(id) != 12) return false;
    bool nonzero = false;
    for (unsigned i = 0; i < 12; ++i)
    {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
        if (c != '0') nonzero = true;
    }
    /* Family identifiers are unicast station MACs. */
    unsigned low = (id[1] <= '9') ? (unsigned)(id[1] - '0') :
                   (unsigned)((id[1] | 32) - 'a' + 10);
    return nonzero && (low & 1u) == 0;
}
static bool valid_type(const char *s)
{
    return s && (!strcmp(s, "tap") || !strcmp(s, "dial") || !strcmp(s, "dial_done") || !strcmp(s, "orbi"));
}
static void make_key(const char *type, uint8_t key[48], size_t *len)
{
    int n = snprintf((char *)key, 48, "ecos/route/%s/%s", s_self, type);
    *len = n > 0 ? (size_t)n : 0;
}
static bool get_target(const char *type, char target[13])
{
    uint8_t key[48], value[13];
    size_t kl = 0, vl = 0;
    make_key(type, key, &kl);
    if (!orbit_state_get(key, kl, value, 12, &vl, NULL, NULL) || vl != 12) return false;
    memcpy(target, value, 12);
    target[12] = 0;
    return valid_id(target);
}
bool link_routes_init(const char *self_id)
{
    if (!valid_id(self_id)) return false;
    char normalized[ROUTE_ID_LEN];
    for (unsigned i = 0; i < 12; ++i)
    {
        normalized[i] = (self_id[i] >= 'A' && self_id[i] <= 'F') ?
                        (char)(self_id[i] - 'A' + 'a') : self_id[i];
    }
    normalized[12] = '\0';
    if (s_self[0] != '\0') return strcmp(s_self, normalized) == 0;
    memcpy(s_self, normalized, sizeof(s_self));
    return true;
}
cJSON *link_routes_config(const cJSON *request)
{
    if (!s_self[0] || !cJSON_IsObject(request)) return NULL;
    unsigned types = 0, targets = 0;
    const cJSON *field = NULL;
    cJSON_ArrayForEach(field, request)
    {
        if (field->string == NULL) return NULL;
        if (strcmp(field->string, "type") == 0) ++types;
        else if (strcmp(field->string, "target") == 0) ++targets;
        else return NULL;
    }
    if (types != 1 || targets > 1) return NULL;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(request, "type");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(request, "target");
    const char *type = cJSON_GetStringValue(t);
    if (!valid_type(type) || (v && !cJSON_IsNull(v) &&
        (!cJSON_IsString(v) || !valid_id(cJSON_GetStringValue(v))))) return NULL;
    uint8_t key[48];
    size_t kl = 0;
    make_key(type, key, &kl);
    bool ok;
    char current[13];
    if (!v)
    {
        cJSON *out = cJSON_CreateObject();
        if (!out) return NULL;
        if (get_target(type, current)) cJSON_AddStringToObject(out, "target", current);
        else cJSON_AddNullToObject(out, "target");
        return out;
    }
    if (cJSON_IsNull(v)) ok = orbit_state_delete(key, kl);
    else
    {
        const char *target = cJSON_GetStringValue(v);
        uint8_t val[12];
        for (unsigned i = 0; i < 12; ++i)
        {
            val[i] = (uint8_t)((target[i] >= 'A' && target[i] <= 'F') ?
                               target[i] - 'A' + 'a' : target[i]);
        }
        ok = orbit_state_put(key, kl, val, 12);
    }
    cJSON *out = cJSON_CreateObject();
    if (!out) return NULL;
    cJSON_AddBoolToObject(out, "ok", ok);
    return out;
}
static void route_worker(void *unused)
{
    (void)unused;
    for (;;)
    {
        portENTER_CRITICAL(&s_mux);
        route_job_t *job = s_head;
        if (job == NULL)
        {
            s_worker_posted = false;
            portEXIT_CRITICAL(&s_mux);
            vTaskDelete(NULL);
            return;
        }
        s_head = job->next;
        if (s_head == NULL) s_tail = NULL;
        --s_count;
        portEXIT_CRITICAL(&s_mux);

        const char *error = NULL;
        if (ecos_family_stream_active()) error = "busy";
        else
        {
            cJSON *command = cJSON_CreateObject();
            if (command != NULL)
            {
                cJSON_AddStringToObject(command, "op", "input");
                cJSON_AddStringToObject(command, "type", job->type);
                if (job->text[0] != '\0') cJSON_AddStringToObject(command, "value", job->text);
                else cJSON_AddNumberToObject(command, "value", job->value);
            }
            char *request = command != NULL ? cJSON_PrintUnformatted(command) : NULL;
            cJSON_Delete(command);
            uint8_t reply[ECOS_ORBIT_MAX];
            int received = -1;
            if (request != NULL && strlen(request) <= ECOS_ORBIT_MAX)
            {
                ecos_family_hold(true);
                received = ecos_family_orbit(job->target, (const uint8_t *)request, strlen(request),
                                               reply, sizeof(reply), ROUTE_TIMEOUT_MS);
                ecos_family_hold(false);
            }
            cJSON_free(request);
            cJSON *answer = received > 0 && (size_t)received <= sizeof(reply) ?
                            link_orbit_parse_json(reply, (size_t)received) : NULL;
            const cJSON *accepted = cJSON_GetObjectItemCaseSensitive(answer, "ok");
            if (!cJSON_IsTrue(accepted)) error = cJSON_IsFalse(accepted) ? "rejected" : "unknown";
            cJSON_Delete(answer);
        }
        heap_caps_free(job);
        if (error != NULL) link_event_text("route_error", error);
    }
}
static void route_discard(void)
{
    portENTER_CRITICAL(&s_mux);
    route_job_t *node = s_head;
    s_head = s_tail = NULL;
    s_count = 0;
    s_worker_posted = false;
    portEXIT_CRITICAL(&s_mux);
    while (node != NULL)
    {
        route_job_t *next = node->next;
        heap_caps_free(node);
        node = next;
    }
    link_event_text("route_error", "busy");
}

static void route_start(void *unused)
{
    (void)unused;
    if (!sys_task_create(route_worker, "link_route", 6144, NULL, 3, SYS_CORE_ANY)) route_discard();
}

void link_routes_emit(const char *type, const char *text, int value)
{
    if (!valid_type(type) || !s_self[0]) return;
    char target[13];
    if (!get_target(type, target) || strcmp(target, s_self) == 0) return;
    route_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job == NULL)
    {
        link_event_text("route_error", "busy");
        return;
    }
    if (strlen(type) >= sizeof(job->type) || (text && strlen(text) >= sizeof(job->text)))
    {
        heap_caps_free(job);
        return;
    }
    strcpy(job->type, type);
    if (text) strcpy(job->text, text);
    job->value = value;
    memcpy(job->target, target, sizeof(job->target));
    portENTER_CRITICAL(&s_mux);
    if (s_count == ROUTE_COUNT)
    {
        portEXIT_CRITICAL(&s_mux);
        heap_caps_free(job);
        link_event_text("route_error", "full");
        return;
    }
    if (s_tail) s_tail->next = job;
    else s_head = job;
    s_tail = job;
    ++s_count;
    bool post = !s_worker_posted;
    if (post) s_worker_posted = true;
    portEXIT_CRITICAL(&s_mux);
    if (post && !sys_post(route_start, NULL)) route_discard();
}
