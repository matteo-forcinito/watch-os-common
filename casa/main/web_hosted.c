/* Casa on the OS web server. Credentials are accepted only by authenticated
 * configuration POSTs and are never included in replies or dashboard state. */
#include "casa.h"
#include "appkit.h"
#include "esp_heap_caps.h"
#include "nvs.h"
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
    cJSON_AddStringToObject(item, "device_class", entity->device_class);
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
    cJSON_AddStringToObject(item, "state", entity->state);
    cJSON_AddStringToObject(item, "display", casa_state_text(entity, display, sizeof(display)));
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
    cJSON_AddNumberToObject(out, "limit", CASA_ENTITIES);
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

static const httpd_uri_t s_routes[] = {
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
