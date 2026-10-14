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

static const char *TAG = "casa";

#define MESSAGE_MAX (96 * 1024)
#define COMMAND_MAX 512
#define URL_MAX 160
#define TOKEN_MAX 300

static SemaphoreHandle_t s_lock;
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

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

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
        snprintf(out, size, "...");
    else if (!e->available)
        snprintf(out, size, "Non disponibile");
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
    if (!e->name[0] && cJSON_IsString(item = cJSON_GetObjectItem(a, "friendly_name")))
        snprintf(e->name, sizeof(e->name), "%s", item->valuestring);
    if (cJSON_IsString(item = cJSON_GetObjectItem(a, "unit_of_measurement")))
        snprintf(e->unit, sizeof(e->unit), "%s", item->valuestring);
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
        e->current_temp = e->target_temp = NAN;
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
        finish_colour(e);
    }
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(event, "r"))
    {
        casa_entity_t *e = cJSON_IsString(item) ? find(item->valuestring) : NULL;
        if (e)
            e->known = e->available = false;
    }
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

static void handle_message(const char *text)
{
    cJSON *msg = cJSON_Parse(text);
    const cJSON *type = cJSON_GetObjectItem(msg, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
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
        const cJSON *success = cJSON_GetObjectItem(msg, "success");
        if (cJSON_IsFalse(success))
        {
            const cJSON *error = cJSON_GetObjectItem(cJSON_GetObjectItem(msg, "error"), "message");
            char text_error[80];
            snprintf(text_error, sizeof(text_error), "Home Assistant: %s", cJSON_IsString(error) ? error->valuestring : "errore");
            set_message(text_error);
        }
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
            s_message_len = 0;
        if (s_message_len + (size_t)d->data_len < MESSAGE_MAX)
        {
            memcpy(s_message + s_message_len, d->data_ptr, d->data_len);
            s_message_len += d->data_len;
        }
        else
            s_message_len = MESSAGE_MAX; /* too big: dropped below */
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
    s_count = 0;
    if (!storage_config_path("casa.json", path, sizeof(path)))
        return;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return;
    char *json = heap_caps_calloc(1, 16 * 1024, MALLOC_CAP_SPIRAM);
    size_t length = json ? fread(json, 1, 16 * 1024 - 1, f) : 0;
    fclose(f);
    cJSON *root = length ? cJSON_Parse(json) : NULL;
    free(json);
    const cJSON *item;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(root, "favorites"))
    {
        const cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!cJSON_IsString(id) || s_count >= CASA_ENTITIES)
            continue;
        casa_entity_t *e = &s_entities[s_count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->id, sizeof(e->id), "%s", id->valuestring);
        const cJSON *name = cJSON_GetObjectItem(item, "name"), *area = cJSON_GetObjectItem(item, "area");
        snprintf(e->name, sizeof(e->name), "%s", cJSON_IsString(name) ? name->valuestring : "");
        snprintf(e->area, sizeof(e->area), "%s", cJSON_IsString(area) && area->valuestring[0] ? area->valuestring : "Altro");
        e->kind = (uint8_t)casa_kind_of(e->id);
        e->brightness = e->position = -1;
        e->current_temp = e->target_temp = NAN;
    }
    s_status.count = s_count;
    s_status.configured = s_url[0] && s_token[0] && s_count > 0;
    s_status.revision++;
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);
}

esp_err_t casa_config_store(const char *json, char *error, size_t error_size)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *url = cJSON_GetObjectItem(root, "url"), *token = cJSON_GetObjectItem(root, "token");
    const cJSON *favorites = cJSON_GetObjectItem(root, "favorites");
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (!cJSON_IsString(url) || (strncmp(url->valuestring, "http://", 7) && strncmp(url->valuestring, "https://", 8)) ||
        strlen(url->valuestring) >= URL_MAX)
        snprintf(error, error_size, "Indirizzo non valido: usa http://... o https://...");
    else if (!cJSON_IsArray(favorites) || cJSON_GetArraySize(favorites) == 0 || cJSON_GetArraySize(favorites) > CASA_ENTITIES)
        snprintf(error, error_size, "Scegli da 1 a %d entità", CASA_ENTITIES);
    else
    {
        /* The token is optional on save: empty keeps the stored one. */
        nvs_handle_t nvs;
        if (nvs_open("casa", NVS_READWRITE, &nvs) == ESP_OK)
        {
            nvs_set_str(nvs, "url", url->valuestring);
            if (cJSON_IsString(token) && token->valuestring[0] && strlen(token->valuestring) < TOKEN_MAX)
                nvs_set_str(nvs, "token", token->valuestring);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        char path[96];
        cJSON *file = cJSON_CreateObject();
        cJSON_AddItemReferenceToObject(file, "favorites", (cJSON *)favorites);
        char *text = cJSON_PrintUnformatted(file);
        cJSON_Delete(file);
        FILE *f = text && storage_config_path("casa.json", path, sizeof(path)) ? fopen(path, "wb") : NULL;
        if (f && fwrite(text, 1, strlen(text), f) == strlen(text))
            result = ESP_OK;
        else
            snprintf(error, error_size, "Nessuna memoria per i file (microSD o flash)");
        if (f)
            fclose(f);
        free(text);
    }
    if (token && cJSON_IsString(token))
        memset(token->valuestring, 0, strlen(token->valuestring));
    cJSON_Delete(root);
    if (result == ESP_OK)
        casa_reconnect();
    return result;
}

void casa_reconnect(void)
{
    atomic_store(&s_reconnect, true);
    if (s_task)
        xTaskNotifyGive(s_task);
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
}

static void casa_task(void *arg)
{
    (void)arg;
    bool online = false;
    uint32_t backoff = 2000, next_ping = 0;
    for (;;)
    {
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
            casa_web_start(); /* the setup page is always reachable while Casa runs */
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
            subscribe();
        /* Outgoing commands. */
        char command[COMMAND_MAX];
        while (atomic_load(&s_authenticated) && xQueueReceive(s_commands, command, 0) == pdTRUE)
            send_text(command);
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
    s_lock = xSemaphoreCreateMutex();
    s_entities = heap_caps_calloc(CASA_ENTITIES, sizeof(casa_entity_t), MALLOC_CAP_SPIRAM);
    s_message = heap_caps_malloc(MESSAGE_MAX + 1, MALLOC_CAP_SPIRAM);
    s_commands = xQueueCreate(8, COMMAND_MAX);
    if (!s_lock || !s_entities || !s_message || !s_commands)
        return ESP_ERR_NO_MEM;
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    load_config();
    atomic_store(&s_dropped, true); /* first connection */
    snprintf(s_status.message, sizeof(s_status.message), "Avvio...");
    if (!sys_task_create(casa_task, "casa", 6144, NULL, 4, tskNO_AFFINITY))
        return ESP_ERR_NO_MEM;
    s_task = xTaskGetHandle("casa");
    return ESP_OK;
}

/* ------------------------------------------------------------ queries and actions */

void casa_status(casa_status_t *out)
{
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
    char command[COMMAND_MAX];
    int n = snprintf(command, sizeof(command),
                     "{\"id\":%d,\"type\":\"call_service\",\"domain\":\"%s\",\"service\":\"%s\","
                     "\"target\":{\"entity_id\":\"%s\"}%s%s}",
                     atomic_fetch_add(&s_next_id, 1), domain, service, entity_id, data_json ? ",\"service_data\":" : "",
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
    default:
        return false;
    }
}
