/*
 * Home Assistant connection and entity model (see casa.h).
 *
 * WebSocket API: auth_required -> auth (long-lived token) -> auth_ok, then
 * subscribe_entities for the favourites only. Home Assistant answers with the
 * full state ("a") and later with compressed changes ("c": "+" adds or
 * replaces, "-" removes attributes, "r" removes entities). Messages can be
 * tens of KB and arrive in pieces: they are reassembled in PSRAM.
 *
 * Commands go through a queue to a sender task: the UI never waits for the
 * network. Ping every 30 s; on loss, reconnect after 2, 4, 8 ... 60 s.
 */
#include "casa.h"
#include "sdkconfig.h"

#include "cJSON.h"
#include "core/sys.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "lvgl.h" /* LV_SYMBOL_RIGHT in state texts */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "services/storage.h"
#include "services/wifi.h"

#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "casa";

#define MESSAGE_MAX (512 * 1024)
#define CONFIG_MAX (192 * 1024)
#define COMMAND_MAX 512
#define URL_MAX 160
#define TOKEN_MAX 300

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_lifecycle;
static atomic_bool s_enabled;
static casa_entity_t *s_entities; /* PSRAM */
static int s_count;
static casa_status_t s_status;
static esp_websocket_client_handle_t s_client;
static char *s_message;      /* reassembly */
static size_t s_message_len;
static QueueHandle_t s_commands;
static atomic_int s_next_id = 10;
static atomic_bool s_authenticated, s_reconnect, s_dropped, s_need_auth, s_need_subscribe;
static char s_url[URL_MAX], s_token[TOKEN_MAX];
static TaskHandle_t s_task;
static atomic_int s_registry_id, s_devices_id;
static atomic_bool s_need_metadata;
static int s_metadata_cursor;
static bool s_optional_message;
static cJSON *s_catalog_request, *s_catalog_result;
static int s_catalog_job, s_catalog_sequence;
static atomic_int s_catalog_id, s_catalog_unsubscribe;
static uint32_t s_catalog_started;
static bool s_catalog_busy, s_catalog_template;
static uint32_t now_ms(void);

/* A bounded, server-owned template reads only public entity metadata. Paging
 * avoids loading the full HA state/registry trees into the panel's RAM. */
bool casa_catalog_request(const char *type, int page, const char *path, int *job)
{
    if (!s_lock || !atomic_load(&s_authenticated) || page < 0 || page > 10000) return false;
    cJSON *request = cJSON_CreateObject();
    if (!strcmp(type, "catalog"))
    {
        static const char template[] =
            "{% set items = states | selectattr('domain','in',"
            "['light','switch','input_boolean','automation','fan','cover','climate','alarm_control_panel',"
            "'media_player','scene','script','button','input_button','lock','sensor','binary_sensor','weather','person','device_tracker']) | list %}"
            "{\"total\":{{ items|count }},\"entities\":["
            "{% for s in items[start:start+64] %}{{ dict(id=s.entity_id,name=s.name,"
            "area=area_name(s.entity_id) or 'Senza stanza',area_id=area_id(s.entity_id) or '',"
            "device_id=device_id(s.entity_id) or '',device_name=device_attr(s.entity_id,'name_by_user') or device_attr(s.entity_id,'name') or '',"
            "device_class=s.attributes.get('device_class',''),state=s.state,unit=s.attributes.get('unit_of_measurement',''),dashboards=[])|tojson }}"
            "{{ ',' if not loop.last else '' }}{% endfor %}]}";
        cJSON_AddStringToObject(request, "type", "render_template");
        cJSON_AddBoolToObject(request, "report_errors", true);
        cJSON_AddStringToObject(request, "template", template);
        cJSON_AddNumberToObject(cJSON_AddObjectToObject(request, "variables"), "start", page * 64);
    }
    else if (!strcmp(type, "lovelace/dashboards/list") || !strcmp(type, "lovelace/config"))
    {
        cJSON_AddStringToObject(request, "type", type);
        if (path && path[0])
        {
            if (strcmp(type, "lovelace/config") || strlen(path) > 128) { cJSON_Delete(request); return false; }
            cJSON_AddStringToObject(request, "url_path", path);
        }
    }
    else { cJSON_Delete(request); return false; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_catalog_busy && now_ms() - s_catalog_started < 30000)
    {
        xSemaphoreGive(s_lock); cJSON_Delete(request); return false;
    }
    cJSON_Delete(s_catalog_request); cJSON_Delete(s_catalog_result);
    s_catalog_result = NULL;
    if (s_catalog_template && atomic_load(&s_catalog_id)) atomic_store(&s_catalog_unsubscribe, atomic_load(&s_catalog_id));
    atomic_store(&s_catalog_id, 0);
    s_catalog_request = request;
    s_catalog_busy = true;
    s_catalog_template = !strcmp(type, "catalog");
    s_catalog_started = now_ms();
    *job = s_catalog_job = ++s_catalog_sequence;
    xSemaphoreGive(s_lock);
    if (s_task) xTaskNotifyGive(s_task);
    return true;
}

cJSON *casa_catalog_poll(int job)
{
    cJSON *out = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_catalog_result != NULL;
    bool valid = job && job == s_catalog_job;
    cJSON_AddBoolToObject(out, "ready", ready || !valid || now_ms() - s_catalog_started >= 30000);
    if (!valid) cJSON_AddStringToObject(out, "error", "Importazione scaduta o sostituita; riprova");
    else if (ready)
    {
        cJSON_AddItemToObject(out, "response", s_catalog_result);
        s_catalog_result = NULL; s_catalog_busy = false;
    }
    else if (now_ms() - s_catalog_started >= 30000)
    {
        cJSON_AddStringToObject(out, "error", "Home Assistant non risponde all'importazione");
        s_catalog_busy = false;
    }
    xSemaphoreGive(s_lock);
    return out;
}

static void catalog_send(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_catalog_busy && now_ms() - s_catalog_started >= 30000)
    {
        if (s_catalog_template) atomic_store(&s_catalog_unsubscribe, atomic_load(&s_catalog_id));
        atomic_store(&s_catalog_id, 0);
        cJSON_Delete(s_catalog_request); s_catalog_request = NULL;
        cJSON_Delete(s_catalog_result); s_catalog_result = NULL;
        s_catalog_busy = false;
    }
    cJSON *request = s_catalog_request; s_catalog_request = NULL;
    if (request)
    {
        int id = atomic_fetch_add(&s_next_id, 1);
        atomic_store(&s_catalog_id, id);
        cJSON_AddNumberToObject(request, "id", id);
    }
    xSemaphoreGive(s_lock);
    if (request)
    {
        char *text = cJSON_PrintUnformatted(request); cJSON_Delete(request);
        if (text) { esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(10000)); free(text); }
    }
    int subscription = atomic_exchange(&s_catalog_unsubscribe, 0);
    if (subscription)
    {
        char text[112];
        snprintf(text, sizeof(text), "{\"id\":%d,\"type\":\"unsubscribe_events\",\"subscription\":%d}", atomic_fetch_add(&s_next_id, 1), subscription);
        esp_websocket_client_send_text(s_client, text, strlen(text), pdMS_TO_TICKS(10000));
    }
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

const char *casa_type_name(const casa_entity_t *e)
{
    if (e->kind == CASA_SENSOR)
    {
        if (!strcmp(e->device_class, "temperature")) return "Termometri";
        if (!strcmp(e->device_class, "humidity")) return "Umidità";
        if (!strcmp(e->device_class, "power") || !strcmp(e->device_class, "energy")) return "Energia";
        return "Sensori";
    }
    switch (e->kind)
    {
    case CASA_LIGHT: return "Luci";
    case CASA_SWITCH: return !strcmp(e->device_class, "outlet") ? "Prese" : "Interruttori";
    case CASA_CLIMATE: return "Termostati";
    case CASA_COVER: return "Tapparelle";
    case CASA_FAN: return "Ventilazione";
    case CASA_MEDIA: return "Multimedia";
    case CASA_SCENE: return "Scene e azioni";
    case CASA_LOCK: return "Serrature";
    case CASA_ALARM: return "Allarme";
    case CASA_PERSON: return "Presenze";
    default: return "Altro";
    }
}

static void set_message(const char *text)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.message, sizeof(s_status.message), "%s", text);
    s_status.revision++;
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------ kinds and text */

casa_kind_t casa_kind_of(const char *id)
{
    static const struct
    {
        const char *prefix;
        casa_kind_t kind;
    } map[] = {
        {"light.", CASA_LIGHT},          {"switch.", CASA_SWITCH},       {"input_boolean.", CASA_SWITCH},
        {"automation.", CASA_SWITCH},    {"fan.", CASA_FAN},             {"cover.", CASA_COVER},
        {"climate.", CASA_CLIMATE},      {"media_player.", CASA_MEDIA},  {"scene.", CASA_SCENE},
        {"script.", CASA_SCENE},         {"button.", CASA_SCENE},        {"input_button.", CASA_SCENE},
        {"lock.", CASA_LOCK},            {"sensor.", CASA_SENSOR},       {"binary_sensor.", CASA_SENSOR},
        {"weather.", CASA_SENSOR},       {"person.", CASA_PERSON},       {"device_tracker.", CASA_PERSON},
        {"alarm_control_panel.", CASA_ALARM},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strncmp(id, map[i].prefix, strlen(map[i].prefix)) == 0)
            return map[i].kind;
    return CASA_OTHER;
}

static void number_text(float value, char *out, size_t size)
{
    /* Italian decimals, one at most; integers stay integers. */
    if (fabsf(value - roundf(value)) < 0.05f)
        snprintf(out, size, "%.0f", value);
    else
    {
        snprintf(out, size, "%.1f", value);
        char *dot = strchr(out, '.');
        if (dot)
            *dot = ',';
    }
}

const char *casa_state_text(const casa_entity_t *e, char *out, size_t size)
{
    char number[16];
    if (!e->known)
        snprintf(out, size, "In attesa di HA");
    else if (!e->available)
        snprintf(out, size, !strcmp(e->state, "unknown") ? "Stato sconosciuto in HA" : "Non disponibile in HA");
    else
        switch (e->kind)
        {
        case CASA_LIGHT:
            if (e->on && e->brightness >= 0)
                snprintf(out, size, "Accesa \xC2\xB7 %d%%", e->brightness);
            else
                snprintf(out, size, e->on ? "Accesa" : "Spenta");
            break;
        case CASA_SWITCH:
            snprintf(out, size, e->on ? "Acceso" : "Spento");
            break;
        case CASA_FAN:
            if (e->on && e->position >= 0)
                snprintf(out, size, "Acceso \xC2\xB7 %d%%", e->position);
            else
                snprintf(out, size, e->on ? "Acceso" : "Spento");
            break;
        case CASA_COVER:
            if (strcmp(e->state, "opening") == 0)
                snprintf(out, size, "Si apre...");
            else if (strcmp(e->state, "closing") == 0)
                snprintf(out, size, "Si chiude...");
            else if (e->on && e->position >= 0 && e->position < 100)
                snprintf(out, size, "Aperta \xC2\xB7 %d%%", e->position);
            else
                snprintf(out, size, e->on ? "Aperta" : "Chiusa");
            break;
        case CASA_CLIMATE:
            if (!isnan(e->current_temp))
            {
                number_text(e->current_temp, number, sizeof(number));
                if (!isnan(e->target_temp) && e->on)
                {
                    char target[16];
                    number_text(e->target_temp, target, sizeof(target));
                    snprintf(out, size, "%s\xC2\xB0 " LV_SYMBOL_RIGHT " %s\xC2\xB0", number, target);
                }
                else
                    snprintf(out, size, "%s\xC2\xB0 \xC2\xB7 %s", number, e->on ? e->state : "spento");
            }
            else
                snprintf(out, size, "%s", e->on ? e->state : "Spento");
            break;
        case CASA_MEDIA:
            if (e->media_title[0] && (e->on || strcmp(e->state, "paused") == 0))
                snprintf(out, size, "%s%s", e->on ? "" : "In pausa \xC2\xB7 ", e->media_title);
            else
                snprintf(out, size, e->on ? "In riproduzione" : strcmp(e->state, "paused") == 0 ? "In pausa"
                                     : strcmp(e->state, "off") == 0 ? "Spento" : "Inattivo");
            break;
        case CASA_SCENE:
            snprintf(out, size, strncmp(e->id, "script.", 7) == 0 ? (e->on ? "In esecuzione" : "Script") : "Tocca per avviare");
            break;
        case CASA_LOCK:
            if (strcmp(e->state, "locked") == 0)
                snprintf(out, size, "Chiusa a chiave");
            else if (strcmp(e->state, "unlocked") == 0)
                snprintf(out, size, "Aperta");
            else
                snprintf(out, size, "%s", e->state);
            break;
        case CASA_PERSON:
            if (strcmp(e->state, "home") == 0)
                snprintf(out, size, "A casa");
            else if (strcmp(e->state, "not_home") == 0)
                snprintf(out, size, "Fuori");
            else
                snprintf(out, size, "%s", e->state); /* a zone name */
            break;
        case CASA_ALARM:
            snprintf(out, size, "%s", !strcmp(e->state, "disarmed") ? "Disinserito" :
                !strcmp(e->state, "armed_away") ? "Fuori casa" : !strcmp(e->state, "armed_home") ? "In casa" :
                !strcmp(e->state, "armed_night") ? "Notte" : !strcmp(e->state, "arming") ? "Inserimento..." :
                !strcmp(e->state, "pending") ? "Ingresso..." : !strcmp(e->state, "triggered") ? "ALLARME" : e->state);
            break;
        default:
        {
            char *end = NULL;
            float value = strtof(e->state, &end);
            if (end && end != e->state && *end == 0)
            {
                number_text(value, number, sizeof(number));
                snprintf(out, size, "%s%s%s", number, e->unit[0] ? " " : "", e->unit);
            }
            else if (strncmp(e->id, "binary_sensor.", 14) == 0)
                snprintf(out, size, e->on ? "Attivo" : "Inattivo");
            else
                snprintf(out, size, "%s", e->state);
            break;
        }
        }
    return out;
}

/* ------------------------------------------------------------ model updates */

static casa_entity_t *find(const char *id)
{
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_entities[i].id, id) == 0)
            return &s_entities[i];
    return NULL;
}

static float number_of(const cJSON *item) { return cJSON_IsNumber(item) ? (float)item->valuedouble : NAN; }

/* Approximate colour of white light, so a warm bulb glows warm on the watch. */
static uint32_t kelvin_rgb(int kelvin)
{
    if (kelvin <= 0)
        return 0xFFC870;
    if (kelvin < 2700)
        return 0xFFB04A;
    if (kelvin < 3500)
        return 0xFFC870;
    if (kelvin < 4500)
        return 0xFFE2B0;
    return 0xE8F0FF;
}

static void apply_attributes(casa_entity_t *e, const cJSON *a)
{
    if (!cJSON_IsObject(a))
        return;
    const cJSON *item;
    if (e->kind == CASA_ALARM)
    {
        if ((item = cJSON_GetObjectItem(a, "code_format"))) e->code_required = cJSON_IsString(item);
        if ((item = cJSON_GetObjectItem(a, "code_arm_required"))) e->code_arm_required = cJSON_IsTrue(item);
        if (cJSON_IsNumber(item = cJSON_GetObjectItem(a, "supported_features"))) e->alarm_features = item->valueint;
    }
    if (!e->name[0] && cJSON_IsString(item = cJSON_GetObjectItem(a, "friendly_name")))
        snprintf(e->name, sizeof(e->name), "%s", item->valuestring);
    if (cJSON_IsString(item = cJSON_GetObjectItem(a, "unit_of_measurement")))
        snprintf(e->unit, sizeof(e->unit), "%s", item->valuestring);
    if (cJSON_IsString(item = cJSON_GetObjectItem(a, "device_class")))
        snprintf(e->device_class, sizeof(e->device_class), "%s", item->valuestring);
    if (cJSON_IsNumber(item = cJSON_GetObjectItem(a, "brightness")))
        e->brightness = (int)lroundf((float)item->valuedouble * 100.0f / 255.0f);
    if (cJSON_IsArray(item = cJSON_GetObjectItem(a, "rgb_color")) && cJSON_GetArraySize(item) == 3)
        e->rgb = (uint32_t)cJSON_GetArrayItem(item, 0)->valueint << 16 | (uint32_t)cJSON_GetArrayItem(item, 1)->valueint << 8 |
                 (uint32_t)cJSON_GetArrayItem(item, 2)->valueint;
    if (cJSON_IsNumber(item = cJSON_GetObjectItem(a, "color_temp_kelvin")))
        e->color_temp_kelvin = item->valueint;
    if (cJSON_IsArray(item = cJSON_GetObjectItem(a, "supported_color_modes")))
    {
        const cJSON *mode;
        e->supports_brightness = e->supports_color = e->supports_temp = false;
        cJSON_ArrayForEach(mode, item)
        {
            const char *m = cJSON_IsString(mode) ? mode->valuestring : "";
            if (strcmp(m, "onoff") != 0)
                e->supports_brightness = true;
            if (!strcmp(m, "rgb") || !strcmp(m, "rgbw") || !strcmp(m, "rgbww") || !strcmp(m, "hs") || !strcmp(m, "xy"))
                e->supports_color = true;
            if (!strcmp(m, "color_temp"))
                e->supports_temp = true;
        }
    }
    if (cJSON_IsNumber(item = cJSON_GetObjectItem(a, "current_position")))
        e->position = item->valueint;
    if (cJSON_IsNumber(item = cJSON_GetObjectItem(a, "percentage")))
        e->position = item->valueint;
    if (cJSON_IsNumber(item = cJSON_GetObjectItem(a, "volume_level")))
        e->position = (int)lroundf((float)item->valuedouble * 100.0f);
    if (cJSON_IsString(item = cJSON_GetObjectItem(a, "media_title")))
        snprintf(e->media_title, sizeof(e->media_title), "%s", item->valuestring);
    if (cJSON_IsString(item = cJSON_GetObjectItem(a, "media_artist")))
        snprintf(e->media_artist, sizeof(e->media_artist), "%s", item->valuestring);
    float value;
    if (!isnan(value = number_of(cJSON_GetObjectItem(a, "current_temperature"))))
        e->current_temp = value;
    if (!isnan(value = number_of(cJSON_GetObjectItem(a, "temperature"))) && e->kind == CASA_CLIMATE)
        e->target_temp = value;
    if (!isnan(value = number_of(cJSON_GetObjectItem(a, "min_temp"))))
        e->min_temp = value;
    if (!isnan(value = number_of(cJSON_GetObjectItem(a, "max_temp"))))
        e->max_temp = value;
    if (!isnan(value = number_of(cJSON_GetObjectItem(a, "target_temp_step"))))
        e->temp_step = value;
    if (cJSON_IsArray(item = cJSON_GetObjectItem(a, "hvac_modes")))
    {
        e->hvac_count = 0;
        const cJSON *mode;
        cJSON_ArrayForEach(mode, item)
            if (cJSON_IsString(mode) && e->hvac_count < 6)
                snprintf(e->hvac_modes[e->hvac_count++], sizeof(e->hvac_modes[0]), "%s", mode->valuestring);
    }
}

static void remove_attributes(casa_entity_t *e, const cJSON *names)
{
    const cJSON *name;
    cJSON_ArrayForEach(name, names)
    {
        const char *n = cJSON_IsString(name) ? name->valuestring : "";
        if (!strcmp(n, "brightness"))
            e->brightness = -1;
        else if (!strcmp(n, "rgb_color"))
            e->rgb = 0;
        else if (!strcmp(n, "color_temp_kelvin"))
            e->color_temp_kelvin = 0;
        else if (!strcmp(n, "media_title"))
            e->media_title[0] = 0;
        else if (!strcmp(n, "media_artist"))
            e->media_artist[0] = 0;
        else if (!strcmp(n, "current_position") || !strcmp(n, "percentage") || !strcmp(n, "volume_level"))
            e->position = -1;
    }
}

static void apply_state(casa_entity_t *e, const char *state)
{
    snprintf(e->state, sizeof(e->state), "%s", state);
    e->known = true;
    e->available = strcmp(state, "unavailable") != 0 && strcmp(state, "unknown") != 0;
    switch (e->kind)
    {
    case CASA_COVER: e->on = !strcmp(state, "open") || !strcmp(state, "opening"); break;
    case CASA_MEDIA: e->on = !strcmp(state, "playing"); break;
    case CASA_CLIMATE: e->on = strcmp(state, "off") != 0 && e->available; break;
    case CASA_LOCK: e->on = !strcmp(state, "unlocked"); break;
    case CASA_PERSON: e->on = !strcmp(state, "home"); break;
    case CASA_ALARM: e->on = e->available && strcmp(state, "disarmed"); break;
    default: e->on = !strcmp(state, "on"); break;
    }
    e->changed_ms = now_ms();
}

static void finish_colour(casa_entity_t *e)
{
    if (e->kind == CASA_LIGHT && e->on && e->rgb == 0)
        e->rgb = kelvin_rgb(e->color_temp_kelvin);
}

/* {"a": {...}, "c": {...}, "r": [...]} from subscribe_entities. */
static void apply_event(const cJSON *event)
{
    const cJSON *item;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(event, "a"))
    {
        casa_entity_t *e = find(item->string);
        if (e == NULL)
            continue;
        e->brightness = e->position = -1;
        e->rgb = 0;
        e->color_temp_kelvin = 0;
        e->media_title[0] = e->media_artist[0] = 0;
        e->current_temp = e->target_temp = e->min_temp = e->max_temp = e->temp_step = NAN;
        const cJSON *state = cJSON_GetObjectItem(item, "s"), *changed = cJSON_GetObjectItem(item, "lc");
        apply_state(e, cJSON_IsString(state) ? state->valuestring : "unknown");
        e->last_changed = cJSON_IsNumber(changed) ? (int64_t)changed->valuedouble : 0;
        apply_attributes(e, cJSON_GetObjectItem(item, "a"));
        finish_colour(e);
    }
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(event, "c"))
    {
        casa_entity_t *e = find(item->string);
        if (e == NULL)
            continue;
        const cJSON *plus = cJSON_GetObjectItem(item, "+"), *minus = cJSON_GetObjectItem(item, "-");
        const cJSON *state = cJSON_GetObjectItem(plus, "s"), *changed = cJSON_GetObjectItem(plus, "lc");
        if (cJSON_IsNumber(changed))
            e->last_changed = (int64_t)changed->valuedouble;
        if (cJSON_IsString(state))
        {
            apply_state(e, state->valuestring);
            if (e->kind == CASA_LIGHT && !e->on)
                e->rgb = 0;
        }
        apply_attributes(e, cJSON_GetObjectItem(plus, "a"));
        remove_attributes(e, cJSON_GetObjectItem(minus, "a"));
        e->changed_ms = now_ms();
        finish_colour(e);
    }
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(event, "r"))
    {
        casa_entity_t *e = cJSON_IsString(item) ? find(item->valuestring) : NULL;
        if (e)
            e->known = e->available = false;
    }
    s_status.event_count++;
    s_status.last_event_ms = now_ms();
    s_status.revision++;
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------ protocol */

static void send_text(const char *text)
{
    if (s_client && esp_websocket_client_is_connected(s_client))
        esp_websocket_client_send_text(s_client, text, (int)strlen(text), pdMS_TO_TICKS(3000));
}

static void subscribe(void)
{
    cJSON *msg = cJSON_CreateObject(), *ids = cJSON_CreateArray();
    cJSON_AddNumberToObject(msg, "id", 1);
    cJSON_AddStringToObject(msg, "type", "subscribe_entities");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++)
        cJSON_AddItemToArray(ids, cJSON_CreateString(s_entities[i].id));
    xSemaphoreGive(s_lock);
    cJSON_AddItemToObject(msg, "entity_ids", ids);
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (text)
    {
        send_text(text);
        free(text);
    }
}


/* Read registry metadata only for selected entities, in bounded batches. */
static void metadata_request(void)
{
    cJSON *request = cJSON_CreateObject();
    int id = atomic_fetch_add(&s_next_id, 1);
    cJSON_AddNumberToObject(request, "id", id);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_metadata_cursor < s_count)
    {
        atomic_store(&s_registry_id, id);
        cJSON_AddStringToObject(request, "type", "config/entity_registry/get_entries");
        cJSON *ids = cJSON_AddArrayToObject(request, "entity_ids");
        int end = s_metadata_cursor + 24;
        if (end > s_count) end = s_count;
        while (s_metadata_cursor < end) cJSON_AddItemToArray(ids, cJSON_CreateString(s_entities[s_metadata_cursor++].id));
    }
    else
    {
        atomic_store(&s_devices_id, id);
        cJSON_AddStringToObject(request, "type", "config/device_registry/list");
    }
    xSemaphoreGive(s_lock);
    char *serialized = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (serialized) { send_text(serialized); free(serialized); }
}

static void metadata_result(const cJSON *message, bool devices)
{
    if (!cJSON_IsTrue(cJSON_GetObjectItem(message, "success")))
    {
        ESP_LOGW(TAG, "Optional HA registry metadata unavailable; imported metadata is retained");
        return;
    }
    const cJSON *result = cJSON_GetObjectItem(message, "result"), *item;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!devices)
    {
        cJSON_ArrayForEach(item, result)
        {
            casa_entity_t *entity = item->string ? find(item->string) : NULL;
            const char *device = cJSON_GetStringValue(cJSON_GetObjectItem(item, "device_id"));
            if (entity && device && strlen(device) < sizeof(entity->device_id))
                snprintf(entity->device_id, sizeof(entity->device_id), "%s", device);
        }
    }
    else
    {
        cJSON_ArrayForEach(item, result)
        {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name_by_user"));
            if (!name || !name[0]) name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
            if (!id || !name) continue;
            size_t length = strlen(name);
            if (length >= CASA_NAME)
            {
                length = CASA_NAME - 1;
                while (length && ((unsigned char)name[length] & 0xC0) == 0x80) length--;
            }
            for (int i = 0; i < s_count; i++)
                if (!strcmp(s_entities[i].device_id, id))
                {
                    memcpy(s_entities[i].device_name, name, length);
                    s_entities[i].device_name[length] = 0;
                }
        }
    }
    s_status.revision++;
    xSemaphoreGive(s_lock);
    if (!devices)
    {
        atomic_store(&s_need_metadata, true);
        if (s_task) xTaskNotifyGive(s_task);
    }
}

static void send_command(const char *command)
{
    /* Allocate IDs when sending, so registry reads, pings and UI commands always
     * reach HA in increasing order, including while metadata is still loading. */
    cJSON *json = cJSON_Parse(command);
    if (!json) return;
    cJSON_ReplaceItemInObject(json, "id", cJSON_CreateNumber(atomic_fetch_add(&s_next_id, 1)));
    char *serialized = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (serialized) { send_text(serialized); free(serialized); }
}

static void handle_message(const char *text)
{
    cJSON *msg = cJSON_Parse(text);
    const cJSON *type = cJSON_GetObjectItem(msg, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    const cJSON *incoming = cJSON_GetObjectItem(msg, "id");
    int incoming_id = cJSON_IsNumber(incoming) ? incoming->valueint : 0;
    if (incoming_id > 0 && incoming_id == atomic_load(&s_catalog_id))
    {
        bool event = !strcmp(t, "event");
        bool failed = cJSON_IsFalse(cJSON_GetObjectItem(msg, "success"));
        if (event || failed || !s_catalog_template)
        {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_catalog_busy)
            {
                cJSON_Delete(s_catalog_result);
                s_catalog_result = cJSON_Duplicate(msg, true);
            }
            xSemaphoreGive(s_lock);
            if (event) atomic_store(&s_catalog_unsubscribe, incoming_id);
            atomic_store(&s_catalog_id, 0);
            if (s_task) xTaskNotifyGive(s_task);
        }
        cJSON_Delete(msg); return;
    }
    if (!strcmp(t, "auth_required"))
    {
        atomic_store(&s_need_auth, true);
        if (s_task)
            xTaskNotifyGive(s_task);
    }
    else if (!strcmp(t, "auth_ok"))
    {
        const cJSON *version = cJSON_GetObjectItem(msg, "ha_version");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_status.ha_version, sizeof(s_status.ha_version), "%s", cJSON_IsString(version) ? version->valuestring : "");
        s_status.connected = true;
        snprintf(s_status.message, sizeof(s_status.message), "Collegato a Home Assistant %s", s_status.ha_version);
        s_status.revision++;
        xSemaphoreGive(s_lock);
        atomic_store(&s_authenticated, true);
        atomic_store(&s_need_subscribe, true);
        if (s_task)
            xTaskNotifyGive(s_task);
    }
    else if (!strcmp(t, "auth_invalid"))
    {
        set_message("Token rifiutato: crea un nuovo token in Home Assistant");
        atomic_store(&s_dropped, true);
    }
    else if (!strcmp(t, "event") && cJSON_GetObjectItem(msg, "id") && cJSON_GetObjectItem(msg, "id")->valueint == 1)
        apply_event(cJSON_GetObjectItem(msg, "event"));
    else if (!strcmp(t, "result"))
    {
        const cJSON *request_id = cJSON_GetObjectItem(msg, "id");
        int id = cJSON_IsNumber(request_id) ? request_id->valueint : 0;
        if (id && (id == atomic_load(&s_registry_id) || id == atomic_load(&s_devices_id)))
        {
            metadata_result(msg, id == atomic_load(&s_devices_id));
            cJSON_Delete(msg);
            return;
        }
        const cJSON *success = cJSON_GetObjectItem(msg, "success");
        if (cJSON_IsFalse(success))
        {
            const cJSON *error = cJSON_GetObjectItem(cJSON_GetObjectItem(msg, "error"), "message");
            char text_error[80];
            snprintf(text_error, sizeof(text_error), "Home Assistant: %s", cJSON_IsString(error) ? error->valuestring : "errore");
            set_message(text_error);
        }
        else if (cJSON_IsTrue(success) && cJSON_GetObjectItem(msg, "id") && cJSON_GetObjectItem(msg, "id")->valueint >= 10)
            set_message("Collegato a Home Assistant");
    }
    cJSON_Delete(msg);
}

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = event_data;
    if (id == WEBSOCKET_EVENT_CONNECTED)
        set_message("Accesso a Home Assistant...");
    else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED || id == WEBSOCKET_EVENT_ERROR)
    {
        atomic_store(&s_authenticated, false);
        atomic_store(&s_dropped, true);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.connected = false;
        s_status.revision++;
        xSemaphoreGive(s_lock);
        if (s_task)
            xTaskNotifyGive(s_task);
    }
    else if (id == WEBSOCKET_EVENT_DATA && (d->op_code == 1 || d->op_code == 0))
    {
        /* One message may arrive in several chunks: payload_offset/payload_len. */
        if (d->payload_offset == 0)
        {
            s_message_len = 0;
            char prefix[96];
            size_t length = d->data_len < sizeof(prefix) - 1 ? d->data_len : sizeof(prefix) - 1;
            memcpy(prefix, d->data_ptr, length);
            prefix[length] = 0;
            const char *id_text = strstr(prefix, "\"id\"");
            id_text = id_text ? strchr(id_text, ':') : NULL;
            int request_id = id_text ? atoi(id_text + 1) : 0;
            s_optional_message = request_id && (request_id == atomic_load(&s_registry_id) || request_id == atomic_load(&s_devices_id) || request_id == atomic_load(&s_catalog_id));
        }
        if (s_message_len + (size_t)d->data_len < MESSAGE_MAX)
        {
            memcpy(s_message + s_message_len, d->data_ptr, d->data_len);
            s_message_len += d->data_len;
        }
        else
        {
            bool first_overflow = s_message_len < MESSAGE_MAX;
            s_message_len = MESSAGE_MAX;
            if (!s_optional_message)
            {
                set_message("Risposta HA troppo grande: riduci le entita' importate");
                atomic_store(&s_dropped, true);
            }
            else if (first_overflow)
                ESP_LOGW(TAG, "Optional registry exceeds the message limit; live states remain connected");
        }
        if (d->payload_offset + d->data_len >= d->payload_len && s_message_len < MESSAGE_MAX)
        {
            s_message[s_message_len] = 0;
            handle_message(s_message);
            s_message_len = 0;
        }
    }
}

/* "homeassistant.local" is not in DNS: resolve it with mDNS and use the address. */
static void resolve_local(const char *url, char *out, size_t size)
{
    snprintf(out, size, "%s", url);
    const char *host = strstr(url, "://");
    if (host == NULL)
        return;
    host += 3;
    size_t length = strcspn(host, ":/");
    if (length < 7 || length > 60 || strncmp(host + length - 6, ".local", 6) != 0)
        return;
    char name[64];
    snprintf(name, sizeof(name), "%.*s", (int)(length - 6), host);
    static bool mdns_ready;
    if (!mdns_ready)
    {
        esp_err_t error = mdns_init();
        mdns_ready = error == ESP_OK || error == ESP_ERR_INVALID_STATE;
    }
    esp_ip4_addr_t address = {0};
    if (!mdns_ready || mdns_query_a(name, 3000, &address) != ESP_OK || address.addr == 0)
    {
        ESP_LOGW(TAG, "%s.local not found by mDNS", name);
        return;
    }
    snprintf(out, size, "%.*s" IPSTR "%s", (int)(host - url), url, IP2STR(&address), host + length);
    ESP_LOGI(TAG, "%s -> " IPSTR, name, IP2STR(&address));
}

/* http(s)://host:port -> ws(s)://host:port/api/websocket */
static void websocket_url(const char *base, char *out, size_t size)
{
    char resolved[URL_MAX + 32];
    resolve_local(base, resolved, sizeof(resolved));
    const char *rest = resolved;
    const char *scheme = "ws://";
    if (strncmp(rest, "https://", 8) == 0)
    {
        scheme = "wss://";
        rest += 8;
    }
    else if (strncmp(rest, "http://", 7) == 0)
        rest += 7;
    size_t n = strlen(rest);
    while (n && rest[n - 1] == '/')
        n--;
    snprintf(out, size, "%s%.*s/api/websocket", scheme, (int)n, rest);
}

/* ------------------------------------------------------------ configuration */

static bool valid_entity_id(const char *id);

static void load_config(void)
{
    nvs_handle_t nvs;
    s_url[0] = s_token[0] = 0;
    if (nvs_open("casa", NVS_READONLY, &nvs) == ESP_OK)
    {
        size_t size = sizeof(s_url);
        nvs_get_str(nvs, "url", s_url, &size);
        size = sizeof(s_token);
        nvs_get_str(nvs, "token", s_token, &size);
        nvs_close(nvs);
    }
    char path[96];
    if (!storage_files_acquire()) return;
    bool has_path = storage_config_path("casa.json", path, sizeof(path));
    if (has_path && access(path, F_OK) != 0)
    {
        char backup[104];
        snprintf(backup, sizeof(backup), "%s.bak", path);
        rename(backup, path); /* Recover an interrupted FAT catalogue replacement. */
    }
    FILE *f = has_path ? fopen(path, "rb") : NULL;
    char *json = f ? heap_caps_calloc(1, CONFIG_MAX, MALLOC_CAP_SPIRAM) : NULL;
    size_t length = json ? fread(json, 1, CONFIG_MAX - 1, f) : 0;
    if (f) fclose(f);
    storage_files_release();
    cJSON *root = length ? cJSON_Parse(json) : NULL;
    free(json);
    const cJSON *item;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(root, "favorites"))
    {
        const cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!cJSON_IsString(id) || !valid_entity_id(id->valuestring) || s_count >= CASA_ENTITIES)
            continue;
        casa_entity_t *e = &s_entities[s_count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->id, sizeof(e->id), "%s", id->valuestring);
        const cJSON *name = cJSON_GetObjectItem(item, "name"), *area = cJSON_GetObjectItem(item, "area");
        snprintf(e->name, sizeof(e->name), "%s", cJSON_IsString(name) ? name->valuestring : "");
        snprintf(e->area, sizeof(e->area), "%s", cJSON_IsString(area) && area->valuestring[0] ? area->valuestring : "Altro");
        const char *device_id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "device_id"));
        const char *device_name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "device_name"));
        snprintf(e->device_id, sizeof(e->device_id), "%s", device_id ? device_id : "");
        snprintf(e->device_name, sizeof(e->device_name), "%s", device_name ? device_name : "");
        const cJSON *device_class = cJSON_GetObjectItem(item, "device_class");
        snprintf(e->device_class, sizeof(e->device_class), "%s", cJSON_IsString(device_class) ? device_class->valuestring : "");
        const cJSON *dashboard;
        int dashboard_index = 0;
        cJSON_ArrayForEach(dashboard, cJSON_GetObjectItem(item, "dashboards"))
        {
            if (cJSON_IsString(dashboard) && dashboard_index < 4)
                snprintf(e->dashboards[dashboard_index++], CASA_NAME, "%s", dashboard->valuestring);
        }
        e->kind = (uint8_t)casa_kind_of(e->id);
        e->brightness = e->position = -1;
        e->current_temp = e->target_temp = e->min_temp = e->max_temp = e->temp_step = NAN;
    }
    s_status.count = s_count;
    s_status.configured = s_url[0] && s_token[0] && s_count > 0;
    s_status.revision++;
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);
}

static bool valid_entity_id(const char *id)
{
    if (id == NULL || strlen(id) >= CASA_ID) return false;
    const char *dot = strchr(id, '.');
    if (dot == NULL || dot == id || dot[1] == 0) return false;
    for (const char *p = id; *p; p++)
        if (p != dot && !((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
            return false;
    return true;
}

esp_err_t casa_config_store(const char *json, char *error, size_t error_size)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *url = cJSON_GetObjectItem(root, "url"), *token = cJSON_GetObjectItem(root, "token");
    const cJSON *favorites = cJSON_GetObjectItem(root, "favorites");
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (!cJSON_IsString(url) || (strncmp(url->valuestring, "http://", 7) && strncmp(url->valuestring, "https://", 8)) ||
        strlen(url->valuestring) >= URL_MAX || strpbrk(url->valuestring, "@?# \r\n\t") != NULL ||
        !strstr(url->valuestring, "://")[3])
        snprintf(error, error_size, "Indirizzo non valido: usa http://... o https://...");
    else if (!cJSON_IsArray(favorites) || cJSON_GetArraySize(favorites) == 0 || cJSON_GetArraySize(favorites) > CASA_ENTITIES)
        snprintf(error, error_size, "Scegli da 1 a %d entità", CASA_ENTITIES);
    else if (token && (!cJSON_IsString(token) || strlen(token->valuestring) >= TOKEN_MAX))
        snprintf(error, error_size, "Token non valido o troppo lungo");
    else
    {
        bool valid = true;
        const cJSON *item;
        cJSON_ArrayForEach(item, favorites)
        {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
            if (!valid_entity_id(id))
            {
                snprintf(error, error_size, "Identificativo non valido o oltre %d caratteri: %.60s", CASA_ID - 1, id ? id : "(mancante)");
                valid = false;
                break;
            }
            for (const cJSON *previous = favorites->child; previous != item; previous = previous->next)
            {
                const char *other = cJSON_GetStringValue(cJSON_GetObjectItem(previous, "id"));
                if (other && strcmp(id, other) == 0)
                {
                    snprintf(error, error_size, "Entita' duplicata: %.80s", id);
                    valid = false;
                    break;
                }
            }
            const cJSON *groups = cJSON_GetObjectItem(item, "dashboards");
            if (groups && (!cJSON_IsArray(groups) || cJSON_GetArraySize(groups) > 4))
            {
                snprintf(error, error_size, "Massimo quattro gruppi dashboard per %.65s", id);
                valid = false;
            }
            const cJSON *group;
            cJSON_ArrayForEach(group, groups)
                if (!cJSON_IsString(group) || strlen(group->valuestring) >= CASA_NAME)
                {
                    snprintf(error, error_size, "Gruppo dashboard non valido o oltre %d byte per %.50s", CASA_NAME - 1, id);
                    valid = false;
                    break;
                }
            if (!valid) break;
        }
        nvs_handle_t nvs;
        if (!valid)
        {
            /* The validation above identifies the offending entity or group. */
        }
        else if (nvs_open("casa", NVS_READWRITE, &nvs) != ESP_OK)
            snprintf(error, error_size, "Impossibile aprire la configurazione");
        else
        {
            char previous_url[URL_MAX] = "";
            size_t length = sizeof(previous_url), token_size = 0;
            nvs_get_str(nvs, "url", previous_url, &length);
            bool has_token = nvs_get_str(nvs, "token", NULL, &token_size) == ESP_OK && token_size > 1;
            bool new_token = cJSON_IsString(token) && token->valuestring[0];
            if ((!has_token || strcmp(previous_url, url->valuestring)) && !new_token)
                snprintf(error, error_size, "Inserisci il token per questo indirizzo Home Assistant");
            else if (!storage_files_acquire())
                snprintf(error, error_size, "Archiviazione occupata, riprova");
            else
            {
                char path[96], temporary[104];
                cJSON *file = cJSON_CreateObject();
                cJSON_AddItemReferenceToObject(file, "favorites", (cJSON *)favorites);
                char *serialized = cJSON_PrintUnformatted(file);
                cJSON_Delete(file);
                bool ok = serialized && strlen(serialized) < CONFIG_MAX && storage_config_path("casa.json", path, sizeof(path));
                FILE *f = NULL;
                if (ok)
                {
                    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
                    f = fopen(temporary, "wb");
                    ok = f != NULL;
                }
                if (f)
                {
                    ok = fwrite(serialized, 1, strlen(serialized), f) == strlen(serialized);
                    ok = fflush(f) == 0 && ok;
                    ok = fsync(fileno(f)) == 0 && ok;
                    ok = fclose(f) == 0 && ok;
                    if (ok)
                    {
                        /* FatFs cannot rename over an existing destination. Keep a
                         * recoverable copy until the new catalogue is in place. */
                        char backup[104];
                        snprintf(backup, sizeof(backup), "%s.bak", path);
                        bool previous = access(path, F_OK) == 0;
                        remove(backup);
                        if (previous) ok = rename(path, backup) == 0;
                        if (ok)
                        {
                            ok = rename(temporary, path) == 0;
                            if (!ok && previous) rename(backup, path);
                            if (ok && previous) remove(backup);
                        }
                    }
                    if (!ok) remove(temporary);
                }
                free(serialized);
                storage_files_release();
                if (ok)
                {
                    result = nvs_set_str(nvs, "url", url->valuestring);
                    if (result == ESP_OK && new_token) result = nvs_set_str(nvs, "token", token->valuestring);
                    if (result == ESP_OK) result = nvs_commit(nvs);
                    if (result != ESP_OK) snprintf(error, error_size, "Salvataggio credenziali non riuscito");
                }
                else snprintf(error, error_size, "Salvataggio catalogo non riuscito");
            }
            nvs_close(nvs);
        }
    }
    if (cJSON_IsString(token)) memset(token->valuestring, 0, strlen(token->valuestring));
    cJSON_Delete(root);
    if (result == ESP_OK) casa_reconnect();
    return result;
}

void casa_reconnect(void)
{
    atomic_store(&s_authenticated, false);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    atomic_store(&s_catalog_id, 0);
    atomic_store(&s_catalog_unsubscribe, 0);
    cJSON_Delete(s_catalog_request); s_catalog_request = NULL;
    cJSON_Delete(s_catalog_result); s_catalog_result = NULL;
    s_catalog_busy = false;
    s_catalog_job = 0;
    s_status.connected = false;
    s_status.revision++;
    xSemaphoreGive(s_lock);
    atomic_store(&s_reconnect, true);
    if (s_task)
        xTaskNotifyGive(s_task);
}

esp_err_t casa_config_clear(void)
{
    if (!storage_files_acquire()) return ESP_ERR_INVALID_STATE;
    nvs_handle_t nvs;
    esp_err_t result = nvs_open("casa", NVS_READWRITE, &nvs);
    if (result == ESP_OK)
    {
        esp_err_t url_error = nvs_erase_key(nvs, "url");
        esp_err_t token_error = nvs_erase_key(nvs, "token");
        if (url_error != ESP_OK && url_error != ESP_ERR_NVS_NOT_FOUND) result = url_error;
        if (token_error != ESP_OK && token_error != ESP_ERR_NVS_NOT_FOUND) result = token_error;
        if (result == ESP_OK) result = nvs_commit(nvs);
        nvs_close(nvs);
    }
    char path[96];
    if (result == ESP_OK && storage_config_path("casa.json", path, sizeof(path)))
    {
        char other[104];
        remove(path);
        snprintf(other, sizeof(other), "%s.bak", path);
        remove(other);
        snprintf(other, sizeof(other), "%s.tmp", path);
        remove(other);
    }
    storage_files_release();
    if (result == ESP_OK) casa_reconnect();
    return result;
}

/* ------------------------------------------------------------ worker */

static void stop_client(void)
{
    if (s_client)
    {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    atomic_store(&s_authenticated, false);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    atomic_store(&s_catalog_id, 0);
    atomic_store(&s_catalog_unsubscribe, 0);
    cJSON_Delete(s_catalog_request); s_catalog_request = NULL;
    cJSON_Delete(s_catalog_result); s_catalog_result = NULL;
    s_catalog_busy = false;
    s_catalog_job = 0;
    s_status.connected = false;
    s_status.revision++;
    xSemaphoreGive(s_lock);
    atomic_store(&s_need_auth, false);
    atomic_store(&s_need_subscribe, false);
    atomic_store(&s_need_metadata, false);
    atomic_store(&s_registry_id, 0);
    atomic_store(&s_devices_id, 0);
    s_metadata_cursor = 0;
    if (s_commands) xQueueReset(s_commands);
}

static void casa_task(void *arg)
{
    (void)arg;
    bool online = false;
    uint32_t backoff = 2000, next_ping = 0;
    for (;;)
    {
        if (!atomic_load(&s_enabled))
        {
            stop_client();
            if (online) wifi_release();
            online = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.connected = s_status.online = false;
            s_status.revision++;
            xSemaphoreGive(s_lock);
            xSemaphoreTake(s_lifecycle, portMAX_DELAY);
            if (!atomic_load(&s_enabled))
            {
                s_task = NULL;
                xSemaphoreGive(s_lifecycle);
                vTaskDelete(NULL);
                return;
            }
            xSemaphoreGive(s_lifecycle);
            atomic_store(&s_dropped, true);
        }
        if (!online)
        {
            set_message("Connessione al Wi-Fi...");
            online = wifi_acquire(20000) == ESP_OK;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.online = online;
            xSemaphoreGive(s_lock);
            if (!online)
            {
                set_message("Wi-Fi non disponibile, riprovo");
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
                continue;
            }
#if !CONFIG_APPKIT_HOSTED
            casa_web_start(); /* Hosted dashboards are registered by the UI lifecycle. */
#endif
        }
        if (atomic_exchange(&s_reconnect, false) || atomic_load(&s_dropped))
        {
            stop_client();
            atomic_store(&s_dropped, false);
            load_config();
            if (!s_status.configured)
            {
                set_message("Configura Casa dal PC");
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
                atomic_store(&s_dropped, true);
                continue;
            }
            char ws[URL_MAX + 48];
            websocket_url(s_url, ws, sizeof(ws));
            esp_websocket_client_config_t config = {
                .uri = ws,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .buffer_size = 4096,
                .task_stack = 8192,
                .network_timeout_ms = 10000,
                .disable_auto_reconnect = true,
            };
            set_message("Connessione a Home Assistant...");
            s_client = esp_websocket_client_init(&config);
            if (s_client == NULL)
            {
                atomic_store(&s_dropped, true);
                continue;
            }
            esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws, NULL);
            if (esp_websocket_client_start(s_client) != ESP_OK)
                atomic_store(&s_dropped, true);
            next_ping = now_ms() + 30000;
        }
        if (atomic_exchange(&s_need_auth, false))
        {
            cJSON *auth = cJSON_CreateObject();
            cJSON_AddStringToObject(auth, "type", "auth");
            cJSON_AddStringToObject(auth, "access_token", s_token);
            char *reply = cJSON_PrintUnformatted(auth);
            cJSON_Delete(auth);
            if (reply)
            {
                send_text(reply);
                memset(reply, 0, strlen(reply)); /* the token does not linger in the heap */
                free(reply);
            }
        }
        if (atomic_exchange(&s_need_subscribe, false))
        {
            subscribe();
            atomic_store(&s_need_metadata, true);
        }
        if (atomic_load(&s_authenticated)) catalog_send();
        if (atomic_load(&s_authenticated) && atomic_exchange(&s_need_metadata, false)) metadata_request();
        /* Outgoing commands. */
        char command[COMMAND_MAX];
        while (atomic_load(&s_authenticated) && xQueueReceive(s_commands, command, 0) == pdTRUE)
            send_command(command);
        if (atomic_load(&s_authenticated))
        {
            backoff = 2000;
            if ((int32_t)(now_ms() - next_ping) >= 0)
            {
                char ping[48];
                snprintf(ping, sizeof(ping), "{\"id\":%d,\"type\":\"ping\"}", atomic_fetch_add(&s_next_id, 1));
                send_text(ping);
                next_ping = now_ms() + 30000;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        }
        else if (atomic_load(&s_dropped))
        {
            char text[64];
            snprintf(text, sizeof(text), "Home Assistant non raggiungibile, riprovo tra %lu s", (unsigned long)(backoff / 1000));
            if (strncmp(s_status.message, "Token", 5) != 0)
                set_message(text);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(backoff));
            backoff = backoff * 2 > 60000 ? 60000 : backoff * 2;
        }
        else
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

static void *psram_malloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

esp_err_t casa_start(void)
{
    if (s_lifecycle == NULL) s_lifecycle = xSemaphoreCreateMutex();
    if (s_lifecycle == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lifecycle, portMAX_DELAY);
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutex();
    if (s_entities == NULL) s_entities = heap_caps_calloc(CASA_ENTITIES, sizeof(casa_entity_t), MALLOC_CAP_SPIRAM);
    if (s_message == NULL) s_message = heap_caps_malloc(MESSAGE_MAX + 1, MALLOC_CAP_SPIRAM);
    if (s_commands == NULL) s_commands = xQueueCreate(8, COMMAND_MAX);
    if (!s_lock || !s_entities || !s_message || !s_commands)
    {
        xSemaphoreGive(s_lifecycle);
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_enabled, true);
    if (s_task != NULL)
    {
        xTaskNotifyGive(s_task);
        xSemaphoreGive(s_lifecycle);
        return ESP_OK;
    }
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    atomic_store(&s_dropped, true); /* first connection */
    snprintf(s_status.message, sizeof(s_status.message), "Avvio...");
    if (xTaskCreatePinnedToCore(casa_task, "casa", 6144, NULL, 4, &s_task, tskNO_AFFINITY) != pdPASS)
    {
        atomic_store(&s_enabled, false);
        xSemaphoreGive(s_lifecycle);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lifecycle);
    return ESP_OK;
}

void casa_stop(void)
{
    if (s_lifecycle == NULL) return;
    xSemaphoreTake(s_lifecycle, portMAX_DELAY);
    atomic_store(&s_enabled, false);
    if (s_task != NULL) xTaskNotifyGive(s_task);
    xSemaphoreGive(s_lifecycle);
}

/* ------------------------------------------------------------ queries and actions */

void casa_status(casa_status_t *out)
{
    if (s_lock == NULL) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

int casa_entities(casa_entity_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count < max ? s_count : max;
    memcpy(out, s_entities, sizeof(casa_entity_t) * (size_t)n);
    xSemaphoreGive(s_lock);
    return n;
}

bool casa_entity(const char *id, casa_entity_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    casa_entity_t *e = find(id);
    if (e)
        *out = *e;
    xSemaphoreGive(s_lock);
    return e != NULL;
}

bool casa_call(const char *domain, const char *service, const char *entity_id, const char *data_json)
{
    casa_entity_t entity;
    if (!casa_entity(entity_id, &entity) || !entity.available || !entity.known) return false;
    if (strlen(domain) != strcspn(entity_id, ".") || strncmp(domain, entity_id, strlen(domain))) return false;
    for (const char *p = service; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || *p == '_')) return false;
    if (service[0] == '\0') return false;
    const char *allowed;
    switch (entity.kind)
    {
    case CASA_LIGHT:
    case CASA_SWITCH:
        allowed = " toggle turn_on turn_off "; break;
    case CASA_FAN:
        allowed = " toggle turn_on turn_off set_percentage "; break;
    case CASA_COVER:
        allowed = " toggle open_cover close_cover stop_cover set_cover_position "; break;
    case CASA_CLIMATE:
        allowed = " turn_on turn_off set_temperature set_hvac_mode "; break;
    case CASA_MEDIA:
        allowed = " media_play_pause media_play media_pause media_stop media_next_track media_previous_track volume_set turn_on turn_off "; break;
    case CASA_SCENE:
        allowed = " turn_on press "; break;
    case CASA_LOCK:
        allowed = " lock unlock "; break;
    case CASA_ALARM:
        allowed = " alarm_disarm alarm_arm_away alarm_arm_home alarm_arm_night alarm_arm_vacation alarm_arm_custom_bypass "; break;
    default:
        return false;
    }
    char needle[64];
    if (snprintf(needle, sizeof(needle), " %s ", service) >= (int)sizeof(needle) || !strstr(allowed, needle)) return false;
    if (entity.kind == CASA_ALARM)
    {
        bool disarm = !strcmp(service, "alarm_disarm");
        int feature = !strcmp(service, "alarm_arm_home") ? 1 : !strcmp(service, "alarm_arm_away") ? 2 :
            !strcmp(service, "alarm_arm_night") ? 4 : !strcmp(service, "alarm_arm_custom_bypass") ? 16 : !strcmp(service, "alarm_arm_vacation") ? 32 : 0;
        if (feature && !(entity.alarm_features & feature)) return false;
        if (entity.code_required && (disarm || entity.code_arm_required))
        {
            cJSON *data = data_json ? cJSON_Parse(data_json) : NULL;
            const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(data, "code"));
            bool provided = code && code[0];
            cJSON_Delete(data);
            if (!provided) return false;
        }
    }
    char command[COMMAND_MAX];
    int n = snprintf(command, sizeof(command),
                     "{\"id\":%d,\"type\":\"call_service\",\"domain\":\"%s\",\"service\":\"%s\","
                     "\"target\":{\"entity_id\":\"%s\"}%s%s}",
                     0, domain, service, entity_id, data_json ? ",\"service_data\":" : "",
                     data_json ? data_json : "");
    if (n <= 0 || n >= (int)sizeof(command) || !atomic_load(&s_authenticated))
        return false;
    bool queued = xQueueSend(s_commands, command, 0) == pdTRUE;
    if (queued && s_task)
        xTaskNotifyGive(s_task);
    return queued;
}

bool casa_tap(const casa_entity_t *e)
{
    char domain[24];
    snprintf(domain, sizeof(domain), "%.*s", (int)strcspn(e->id, "."), e->id);
    switch (e->kind)
    {
    case CASA_LIGHT:
    case CASA_SWITCH:
    case CASA_FAN:
    case CASA_COVER:
        return casa_call(domain, "toggle", e->id, NULL);
    case CASA_CLIMATE:
        return casa_call(domain, e->on ? "turn_off" : "turn_on", e->id, NULL);
    case CASA_MEDIA:
        return casa_call(domain, "media_play_pause", e->id, NULL);
    case CASA_SCENE:
        return casa_call(domain, !strcmp(domain, "button") || !strcmp(domain, "input_button") ? "press" : "turn_on", e->id, NULL);
    case CASA_LOCK:
        return casa_call(domain, strcmp(e->state, "locked") == 0 ? "unlock" : "lock", e->id, NULL);
    case CASA_ALARM:
        if (strcmp(e->state, "disarmed") && strcmp(e->state, "armed_away")) return false;
        return casa_call(domain, !strcmp(e->state, "disarmed") ? "alarm_arm_away" : "alarm_disarm", e->id, NULL);
    default:
        return false;
    }
}
