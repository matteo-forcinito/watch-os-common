/* Casa on the OS web server. Credentials are accepted only by authenticated
 * configuration POSTs and are never included in replies or dashboard state. */
#include "casa.h"
#include "appkit.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "core/settings.h"
#include "services/weather.h"
#include "services/alarm.h"
#include "services/storage.h"
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char casa_dashboard_start[] asm("_binary_casa_dashboard_html_start");
extern const char casa_dashboard_end[] asm("_binary_casa_dashboard_html_end");

static cJSON *entity_json(const casa_entity_t *entity, bool configuration)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", entity->id);
    cJSON_AddStringToObject(item, "name", entity->name);
    cJSON_AddStringToObject(item, "area", entity->area);
    cJSON_AddStringToObject(item, "device_id", entity->device_id);
    cJSON_AddStringToObject(item, "device_name", entity->device_name);
    cJSON_AddStringToObject(item, "device_class", entity->device_class);
    if (entity->indoor_mode) cJSON_AddBoolToObject(item, "indoor", entity->indoor_mode == 1);
    cJSON_AddStringToObject(item, "category", casa_type_name(entity));
    cJSON *groups = cJSON_AddArrayToObject(item, "dashboards");
    for (int i = 0; i < 4; i++)
        if (entity->dashboards[i][0]) cJSON_AddItemToArray(groups, cJSON_CreateString(entity->dashboards[i]));
    if (configuration) return item;
    char display[80];
    cJSON_AddNumberToObject(item, "kind", entity->kind);
    cJSON_AddBoolToObject(item, "known", entity->known);
    cJSON_AddBoolToObject(item, "available", entity->available);
    cJSON_AddBoolToObject(item, "on", entity->on);
    cJSON_AddBoolToObject(item, "code_required", entity->code_required);
    cJSON_AddBoolToObject(item, "code_arm_required", entity->code_arm_required);
    cJSON_AddNumberToObject(item, "alarm_features", entity->alarm_features);
    cJSON_AddStringToObject(item, "state", entity->state);
    cJSON_AddStringToObject(item, "display", casa_state_text(entity, display, sizeof(display)));
    cJSON_AddNumberToObject(item, "received_ms", entity->changed_ms);
    cJSON_AddStringToObject(item, "unit", entity->unit);
    cJSON_AddNumberToObject(item, "brightness", entity->brightness);
    cJSON_AddNumberToObject(item, "position", entity->position);
    cJSON_AddNumberToObject(item, "current_temp", entity->current_temp);
    cJSON_AddNumberToObject(item, "target_temp", entity->target_temp);
    cJSON_AddNumberToObject(item, "min_temp", entity->min_temp);
    cJSON_AddNumberToObject(item, "max_temp", entity->max_temp);
    cJSON_AddNumberToObject(item, "temp_step", entity->temp_step);
    cJSON_AddBoolToObject(item, "supports_brightness", entity->supports_brightness);
    cJSON *modes = cJSON_AddArrayToObject(item, "hvac_modes");
    for (int i = 0; i < entity->hvac_count; i++) cJSON_AddItemToArray(modes, cJSON_CreateString(entity->hvac_modes[i]));
    return item;
}

static esp_err_t snapshot(httpd_req_t *request)
{
    if (!appkit_authorized(request)) return appkit_unauthorized(request);
    bool configuration = strstr(request->uri, "/api/config") != NULL;
    casa_status_t status;
    casa_status(&status);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "connected", status.connected);
    cJSON_AddBoolToObject(out, "configured", status.configured);
    cJSON_AddStringToObject(out, "message", status.message);
    cJSON_AddNumberToObject(out, "revision", status.revision);
    cJSON_AddNumberToObject(out, "event_count", status.event_count);
    cJSON_AddNumberToObject(out, "last_event_ms", status.last_event_ms);
    cJSON_AddNumberToObject(out, "limit", CASA_ENTITIES);
    cJSON_AddNumberToObject(out, "homepage", settings_get()->homepage);
    alarm_t alarm;
    time_t when;
    if (alarm_next(&alarm, &when))
    {
        struct tm local;
        localtime_r(&when, &local);
        char time[8];
        strftime(time, sizeof(time), "%H:%M", &local);
        cJSON_AddStringToObject(out, "next_alarm", time);
    }
    weather_t weather;
    if (weather_get(&weather))
    {
        cJSON *forecast = cJSON_AddObjectToObject(out, "weather");
        cJSON_AddNumberToObject(forecast, "code", weather.code);
        cJSON_AddNumberToObject(forecast, "temperature", weather.temp);
        cJSON_AddStringToObject(forecast, "city", weather.city);
        cJSON_AddBoolToObject(forecast, "is_day", weather.is_day);
    }
    char url[160] = "";
    bool has_token = false;
    nvs_handle_t nvs;
    if (nvs_open("casa", NVS_READONLY, &nvs) == ESP_OK)
    {
        size_t size = sizeof(url);
        nvs_get_str(nvs, "url", url, &size);
        size = 0;
        has_token = nvs_get_str(nvs, "token", NULL, &size) == ESP_OK && size > 1;
        nvs_close(nvs);
    }
    cJSON_AddStringToObject(out, "url", url);
    cJSON_AddBoolToObject(out, "has_token", has_token);
    casa_entity_t *entities = heap_caps_calloc(CASA_ENTITIES, sizeof(*entities), MALLOC_CAP_SPIRAM);
    if (entities == NULL) { cJSON_Delete(out); return appkit_error(request, "503 Service Unavailable", "Memoria insufficiente"); }
    int count = casa_entities(entities, CASA_ENTITIES);
    casa_home_stats_t stats;
    casa_home_stats(entities, count, &stats);
    cJSON *home = cJSON_AddObjectToObject(out, "home");
    cJSON_AddNumberToObject(home, "temperature", stats.temperature);
    cJSON_AddNumberToObject(home, "humidity", stats.humidity);
    cJSON_AddNumberToObject(home, "power_w", stats.power_w);
    cJSON_AddNumberToObject(home, "temperatures", stats.temperatures);
    cJSON_AddNumberToObject(home, "humidities", stats.humidities);
    cJSON_AddNumberToObject(home, "meters", stats.meters);
    cJSON_AddNumberToObject(home, "meters_available", stats.meters_available);
    int known = 0, available = 0;
    for (int i = 0; i < count; i++)
    {
        known += entities[i].known;
        available += entities[i].known && entities[i].available;
    }
    cJSON_AddNumberToObject(out, "known", known);
    cJSON_AddNumberToObject(out, "available", available);
    cJSON *list = cJSON_AddArrayToObject(out, configuration ? "favorites" : "entities");
    for (int i = 0; i < count; i++) cJSON_AddItemToArray(list, entity_json(&entities[i], configuration));
    free(entities);
    return appkit_json(request, out);
}

static esp_err_t configure(httpd_req_t *request)
{
    if (!appkit_authorized(request)) return appkit_unauthorized(request);
    char *text = appkit_body(request, 192 * 1024);
    if (text == NULL) return appkit_error(request, "400 Bad Request", "Configurazione vuota o troppo grande");
    char error[128] = "";
    esp_err_t result = casa_config_store(text, error, sizeof(error));
    memset(text, 0, strlen(text));
    free(text);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", result == ESP_OK);
    if (result != ESP_OK) cJSON_AddStringToObject(out, "error", error);
    return appkit_json(request, out);
}

static esp_err_t action(httpd_req_t *request)
{
    if (!appkit_authorized(request)) return appkit_unauthorized(request);
    char *text = appkit_body(request, 2048);
    cJSON *json = text ? cJSON_Parse(text) : NULL;
    free(text);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(json, "id"));
    const char *service = cJSON_GetStringValue(cJSON_GetObjectItem(json, "service"));
    const cJSON *data = cJSON_GetObjectItem(json, "data");
    casa_entity_t *entity = heap_caps_malloc(sizeof(*entity), MALLOC_CAP_SPIRAM);
    bool ok = id && entity && casa_entity(id, entity);
    if (ok && service == NULL) ok = casa_tap(entity);
    else if (ok)
    {
        ok = (!data || cJSON_IsObject(data)) &&
             !cJSON_GetObjectItem(data, "entity_id") && !cJSON_GetObjectItem(data, "area_id") && !cJSON_GetObjectItem(data, "device_id");
        char *serialized = data && ok ? cJSON_PrintUnformatted(data) : NULL;
        char domain[32];
        snprintf(domain, sizeof(domain), "%.*s", (int)strcspn(id, "."), id);
        ok = ok && (!data || serialized) && casa_call(domain, service, id, serialized);
        free(serialized);
    }
    free(entity);
    cJSON_Delete(json);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    cJSON_AddStringToObject(out, "message", ok ? "Comando inviato; attendo lo stato da Home Assistant" : "Entità non disponibile o comando non inviato");
    return appkit_json(request, out);
}

static esp_err_t disconnect(httpd_req_t *request)
{
    if (!appkit_authorized(request)) return appkit_unauthorized(request);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", casa_config_clear() == ESP_OK);
    return appkit_json(request, out);
}

static esp_err_t catalog(httpd_req_t *request)
{
    if (!appkit_authorized(request)) return appkit_unauthorized(request);
    if (request->method == HTTP_GET)
    {
        const char *query = strchr(request->uri, '?');
        int job = query && !strncmp(query, "?job=", 5) ? atoi(query + 5) : 0;
        return appkit_json(request, casa_catalog_poll(job));
    }
    char *text = appkit_body(request, 512);
    cJSON *json = text ? cJSON_Parse(text) : NULL;
    free(text);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(json, "type"));
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(json, "url_path"));
    const cJSON *page = cJSON_GetObjectItem(json, "page");
    int job = 0;
    bool ok = type && casa_catalog_request(type, cJSON_IsNumber(page) ? page->valueint : 0, path, &job);
    cJSON_Delete(json);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    cJSON_AddNumberToObject(out, "job", job);
    if (!ok) cJSON_AddStringToObject(out, "error", "Attendi il collegamento Home Assistant o la fine dell'importazione in corso");
    return appkit_json(request, out);
}

static esp_err_t home_preferences(httpd_req_t *request)
{
    if (!appkit_authorized(request)) return appkit_unauthorized(request);
    char *text = appkit_body(request, 128);
    cJSON *json = text ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *choice = cJSON_GetObjectItem(json, "homepage");
    bool ok = cJSON_IsNumber(choice) && (choice->valuedouble == 0 || choice->valuedouble == 1);
    if (ok)
    {
        settings_t settings = *settings_get();
        settings.homepage = choice->valueint;
        settings_save(&settings);
    }
    cJSON_Delete(json);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    return appkit_json(request, out);
}

/* Metadata is cached on SD, or the board's internal FAT volume. Live states
 * remain on the authenticated HA subscription; the cache is never resident. */
#define CATALOG_BYTES (512 * 1024)
static bool cache_source(char *url, size_t capacity)
{
    nvs_handle_t nvs;
    if (nvs_open("casa", NVS_READONLY, &nvs) != ESP_OK) return false;
    esp_err_t result = nvs_get_str(nvs, "url", url, &capacity);
    nvs_close(nvs);
    return result == ESP_OK && url[0];
}

static bool cache_entry(cJSON *entry)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "id"));
    if (!id || !id[0] || strlen(id) > 255 || !strchr(id, '.') || casa_kind_of(id) == CASA_OTHER) return false;
    for (const char *p = id; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.') return false;
    const char *keys[] = {"id", "name", "area", "area_id", "device_id", "device_name", "device_class", "dashboards", "state", "unit"};
    cJSON *field = entry->child;
    while (field)
    {
        cJSON *next = field->next;
        bool allowed = false;
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) if (!strcmp(field->string, keys[i])) allowed = true;
        if (!allowed) cJSON_DeleteItemFromObject(entry, field->string);
        else if (!strcmp(field->string, "dashboards"))
        {
            if (!cJSON_IsArray(field) || cJSON_GetArraySize(field) > 4) return false;
            const cJSON *group;
            cJSON_ArrayForEach(group, field) if (!cJSON_IsString(group) || strlen(group->valuestring) > 160) return false;
        }
        else if (!cJSON_IsString(field) || strlen(field->valuestring) > 255) return false;
        field = next;
    }
    return true;
}

static esp_err_t catalog_cache(httpd_req_t *request)
{
    if (!appkit_authorized(request)) return appkit_unauthorized(request);
    char source[160] = "", path[96], temporary[104], backup[104];
    if (!cache_source(source, sizeof(source))) return appkit_error(request, "404 Not Found", "Collega Home Assistant prima di salvare il catalogo");
    cJSON *json = NULL;
    if (request->method == HTTP_POST)
    {
        char *body = appkit_body(request, CATALOG_BYTES);
        cJSON *incoming = body ? cJSON_Parse(body) : NULL;
        free(body);
        cJSON *entries = cJSON_GetObjectItem(incoming, "entities");
        bool valid = cJSON_IsArray(entries) && cJSON_GetArraySize(entries) > 0 && cJSON_GetArraySize(entries) <= 4096;
        cJSON *entry;
        cJSON_ArrayForEach(entry, entries) if (valid && !cache_entry(entry)) valid = false;
        if (valid)
        {
            json = cJSON_CreateObject();
            if (json)
            {
                cJSON *owned = cJSON_DetachItemFromObject(incoming, "entities");
                if (!cJSON_AddItemToObject(json, "entities", owned))
                {
                    cJSON_Delete(owned);
                    cJSON_Delete(json);
                    json = NULL;
                }
            }
            if (json && (!cJSON_AddStringToObject(json, "source", source) ||
                         !cJSON_AddNumberToObject(json, "schema", 1) ||
                         !cJSON_AddNumberToObject(json, "saved_at", (double)time(NULL))))
            {
                cJSON_Delete(json);
                json = NULL;
            }
        }
        cJSON_Delete(incoming);
        if (!json) return appkit_error(request, valid ? "503 Service Unavailable" : "400 Bad Request", valid ? "Memoria occupata; riprova" : "Catalogo non valido o troppo grande");
    }
    if (!storage_files_acquire()) { cJSON_Delete(json); return appkit_error(request, "503 Service Unavailable", "Archiviazione occupata; riprova"); }
    bool ok = storage_config_path("casa-catalog.json", path, sizeof(path));
    if (ok)
    {
        snprintf(temporary, sizeof(temporary), "%s.tmp", path);
        snprintf(backup, sizeof(backup), "%s.bak", path);
        if (request->method == HTTP_GET)
        {
            if (access(path, F_OK) != 0 && access(backup, F_OK) == 0) rename(backup, path);
            FILE *file = fopen(path, "rb");
            char *text = file ? heap_caps_calloc(1, CATALOG_BYTES + 1, MALLOC_CAP_SPIRAM) : NULL;
            size_t length = text ? fread(text, 1, CATALOG_BYTES, file) : 0;
            bool overflow = file && text && fgetc(file) != EOF;
            if (file) fclose(file);
            json = length && !overflow ? cJSON_Parse(text) : NULL;
            free(text);
            const char *saved_source = cJSON_GetStringValue(cJSON_GetObjectItem(json, "source"));
            ok = saved_source && !strcmp(saved_source, source) && cJSON_IsArray(cJSON_GetObjectItem(json, "entities"));
        }
        else
        {
            char *text = cJSON_PrintUnformatted(json);
            size_t length = text ? strlen(text) : 0;
            FILE *file = length && length <= CATALOG_BYTES ? fopen(temporary, "wb") : NULL;
            ok = file != NULL;
            if (file)
            {
                ok = fwrite(text, 1, length, file) == length;
                ok = fflush(file) == 0 && ok;
                ok = fsync(fileno(file)) == 0 && ok;
                ok = fclose(file) == 0 && ok;
                if (ok)
                {
                    if (access(path, F_OK) != 0 && access(backup, F_OK) == 0) rename(backup, path);
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
            free(text);
        }
    }
    storage_files_release();
    if (!ok) { cJSON_Delete(json); return appkit_error(request, request->method == HTTP_GET ? "404 Not Found" : "503 Service Unavailable", "Catalogo salvato non disponibile; reimporta o verifica l'archiviazione"); }
    if (request->method == HTTP_GET) return appkit_json(request, json);
    cJSON_Delete(json);
    json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    return appkit_json(request, json);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/catalog/cache", .method = HTTP_GET, .handler = catalog_cache},
    {.uri = "/api/catalog/cache", .method = HTTP_POST, .handler = catalog_cache},
    {.uri = "/api/home", .method = HTTP_POST, .handler = home_preferences},
    {.uri = "/api/catalog", .method = HTTP_POST, .handler = catalog},
    {.uri = "/api/catalog", .method = HTTP_GET, .handler = catalog},
    {.uri = "/api/config", .method = HTTP_GET, .handler = snapshot},
    {.uri = "/api/config", .method = HTTP_POST, .handler = configure},
    {.uri = "/api/disconnect", .method = HTTP_POST, .handler = disconnect},
    {.uri = "/api/state", .method = HTTP_GET, .handler = snapshot},
    {.uri = "/api/action", .method = HTTP_POST, .handler = action},
};
static const appkit_web_config_t s_config = {
    .name = "Casa",
    .page_start = casa_dashboard_start,
    .page_end = casa_dashboard_end,
    .routes = s_routes,
    .route_count = sizeof(s_routes) / sizeof(s_routes[0]),
};

esp_err_t casa_web_start(void) { return appkit_web_start(&s_config); }
const char *casa_web_code(void) { return appkit_web_code(); }
const char *casa_web_address(void)
{
    static char address[96];
    appkit_web_url(address, sizeof(address));
    return address;
}
