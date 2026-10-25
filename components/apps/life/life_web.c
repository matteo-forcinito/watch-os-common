#include "life_web.h"
#include "life_store.h"
#include <stdlib.h>

extern const char waste_start[] asm("_binary_raccolta_dashboard_html_start");
extern const char waste_end[] asm("_binary_raccolta_dashboard_html_end");
extern const char dogs_start[] asm("_binary_zampe_dashboard_html_start");
extern const char dogs_end[] asm("_binary_zampe_dashboard_html_end");

static esp_err_t state(httpd_req_t *request)
{
    if (!appkit_authorized(request))
        return appkit_unauthorized(request);
    life_kind_t kind = (life_kind_t)(uintptr_t)request->user_ctx;
    cJSON *out = life_snapshot(kind);
    if (!out)
        return appkit_error(request, "503 Service Unavailable", "Archivio non disponibile");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return appkit_json(request, out);
}
static esp_err_t action(httpd_req_t *request)
{
    if (!appkit_authorized(request))
        return appkit_unauthorized(request);
    char *raw = appkit_body(request, 4096);
    cJSON *command = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    char error[128] = "Richiesta non valida";
    bool ok = cJSON_IsObject(command) &&
              life_command((life_kind_t)(uintptr_t)request->user_ctx, command, error, sizeof(error));
    cJSON_Delete(command);
    if (!ok)
        return appkit_error(request, "400 Bad Request", error);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "accepted", true);
    /* Queued is not durable: state.saving/save_error report the write result. */
    cJSON_AddStringToObject(out, "status", "queued");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return appkit_json(request, out);
}
static esp_err_t export_data(httpd_req_t *request)
{
    if (!appkit_authorized(request))
        return appkit_unauthorized(request);
    life_kind_t kind = (life_kind_t)(uintptr_t)request->user_ctx;
    cJSON *out = life_snapshot(kind);
    if (!out)
        return appkit_error(request, "503 Service Unavailable", "Archivio non disponibile");
    cJSON_DeleteItemFromObjectCaseSensitive(out, "runtime");
    cJSON_DeleteItemFromObjectCaseSensitive(out, "revision");
    cJSON_DeleteItemFromObjectCaseSensitive(out, "saving");
    cJSON_DeleteItemFromObjectCaseSensitive(out, "save_error");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Content-Disposition",
                       kind == LIFE_ZAMPE ? "attachment; filename=zampe-private.json"
                                          : "attachment; filename=raccolta.json");
    return appkit_json(request, out);
}
#define ROUTES(kind)                                                                                                   \
    {.uri = "/api/state", .method = HTTP_GET, .handler = state, .user_ctx = (void *)(uintptr_t)kind},                  \
        {.uri = "/api/action", .method = HTTP_POST, .handler = action, .user_ctx = (void *)(uintptr_t)kind},           \
    {                                                                                                                  \
        .uri = "/api/export", .method = HTTP_GET, .handler = export_data, .user_ctx = (void *)(uintptr_t)kind          \
    }
static const httpd_uri_t waste_routes[] = {ROUTES(LIFE_RACCOLTA)};
static const httpd_uri_t dogs_routes[] = {ROUTES(LIFE_ZAMPE)};
static const appkit_web_config_t configs[] = {
    {.name = "Raccolta", .page_start = waste_start, .page_end = waste_end, .routes = waste_routes, .route_count = 3},
    {.name = "Zampe", .page_start = dogs_start, .page_end = dogs_end, .routes = dogs_routes, .route_count = 3}};
const appkit_web_config_t *life_web_config(life_kind_t kind)
{
    return (unsigned)kind < LIFE_COUNT ? &configs[kind] : NULL;
}
