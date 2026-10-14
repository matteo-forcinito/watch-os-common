/*
 * System dashboard API (paired browsers only, see web_server.c):
 *
 *   GET  /api/system           device, battery, memory, storage, Wi-Fi, versions
 *   GET  /api/apps             built-in + external apps, favourites, cards per page
 *   POST /api/apps/open        {id} built-in or {ext: id} external: opens it on the watch
 *   POST /api/apps/launcher    {favorites:[...], cards: 4|6|9}
 *   GET  /api/market           status + catalogue (with installed versions)
 *   POST /api/market/refresh   POST /api/market/install {id}   POST /api/market/remove {id}
 *   POST /api/market/update_all  every app with a newer version, one at a time
 *   GET  /api/update           system update state; POST /api/update/check, /api/update/install
 *   POST /api/reboot
 */
#include "web_files.h"

#include "core/app.h"
#include "core/lv_port.h"
#include "core/settings.h"
#include "core/sys.h"
#include "hardware/board.h"
#include "hardware/pmu.h"
#include "hardware/sdcard.h"
#include "services/extapp.h"
#include "services/fw_update.h"
#include "services/fw_version.h"
#include "services/market.h"
#include "services/storage.h"
#include "services/web_server.h"
#include "services/wifi.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_bool s_update_all;

static esp_err_t send_out(httpd_req_t *req, cJSON *json)
{
    char *text = json != NULL ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text != NULL ? text : "{}");
    cJSON_free(text);
    return err;
}

static esp_err_t reply(httpd_req_t *req, bool ok, const char *message)
{
    if (!ok)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, message);
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "message", message);
    return send_out(req, json);
}

static cJSON *body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 4096)
    {
        return NULL;
    }
    char *text = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    if (text == NULL)
    {
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len)
    {
        int n = httpd_req_recv(req, text + got, req->content_len - got);
        if (n <= 0)
        {
            free(text);
            return NULL;
        }
        got += (size_t)n;
    }
    text[got] = '\0';
    cJSON *json = cJSON_Parse(text);
    free(text);
    return json;
}

/* ------------------------------------------------------------- system */

static esp_err_t system_get(httpd_req_t *req)
{
    const board_info_t *b = board_info();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "product", b->product);
    cJSON_AddStringToObject(out, "board", b->name);
    cJSON_AddStringToObject(out, "name", sys_nickname());
    cJSON_AddBoolToObject(out, "stop", true);   /* kit.js: power button */
    char host[24];
    snprintf(host, sizeof(host), "%s.local", sys_hostname());
    cJSON_AddStringToObject(out, "hostname", host);
    cJSON_AddBoolToObject(out, "round", b->round);
    cJSON_AddNumberToObject(out, "width", b->width);
    cJSON_AddNumberToObject(out, "height", b->height);
    cJSON_AddStringToObject(out, "version", fw_update_running_version());
    cJSON_AddNumberToObject(out, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(out, "battery", pmu_present() ? pmu_battery_percent() : -1);
    cJSON_AddNumberToObject(out, "battery_mv", pmu_present() ? pmu_battery_mv() : 0);
    cJSON_AddBoolToObject(out, "charging", pmu_present() && pmu_is_charging());
    cJSON_AddBoolToObject(out, "usb", pmu_present() && pmu_usb_present());

    cJSON *mem = cJSON_AddObjectToObject(out, "memory");
    cJSON_AddNumberToObject(mem, "internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(mem, "psram_total", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

    cJSON *disk = cJSON_AddObjectToObject(out, "storage");
    cJSON_AddBoolToObject(disk, "sd_slot", b->has_sdcard);
    cJSON_AddBoolToObject(disk, "sd", sdcard_is_mounted());
    if (sdcard_is_mounted())
    {
        uint32_t total = 0, free_mb = 0;
        sdcard_space(&total, &free_mb);
        cJSON_AddNumberToObject(disk, "sd_total_mb", total);
        cJSON_AddNumberToObject(disk, "sd_free_mb", free_mb);
    }
    if (storage_flash_mounted())
    {
        size_t total = 0, free_kb = 0;
        storage_flash_space(&total, &free_kb);
        cJSON_AddNumberToObject(disk, "flash_total_kb", total);
        cJSON_AddNumberToObject(disk, "flash_free_kb", free_kb);
    }

    cJSON *net = cJSON_AddObjectToObject(out, "wifi");
    char text[40] = "";
    wifi_current_ssid(text, sizeof(text));
    cJSON_AddStringToObject(net, "ssid", text);
    cJSON_AddNumberToObject(net, "rssi", wifi_rssi());
    text[0] = '\0';
    wifi_get_ip(text, sizeof(text));
    cJSON_AddStringToObject(net, "ip", text);
    return send_out(req, out);
}

static void restart_job(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

/* The page's power button: close the Web page app on the watch (Wi-Fi off). */
static esp_err_t kit_off(httpd_req_t *req)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    esp_err_t err = send_out(req, out);
    lv_port_lock();
    const app_t *app = app_current();
    if (app != NULL && strcmp(app->id, "web") == 0)
    {
        app_back();
    }
    lv_port_unlock();
    return err;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    esp_err_t err = reply(req, true, "Riavvio...");
    sys_post(restart_job, NULL);
    return err;
}

/* --------------------------------------------------------------- apps */

static esp_err_t apps_get(httpd_req_t *req)
{
    cJSON *out = cJSON_CreateObject();
    cJSON *builtin = cJSON_AddArrayToObject(out, "builtin");
    for (size_t i = 0; i < app_count(); i++)
    {
        const app_t *a = app_at(i);
        if (a->flags & APP_FLAG_HIDDEN)
        {
            continue;
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "id", a->id);
        cJSON_AddStringToObject(j, "name", a->name);
        cJSON_AddNumberToObject(j, "color", a->color);
        cJSON_AddStringToObject(j, "about", a->about != NULL ? a->about : "");
        char folder[32];
        if (web_server_app_folder(a->id, folder, sizeof(folder)) != NULL)
        {
            cJSON_AddStringToObject(j, "dashboard", folder);   /* opening it opens the app */
        }
        cJSON_AddItemToArray(builtin, j);
    }
    extapp_t *ext = heap_caps_calloc(EXTAPP_MAX, sizeof(extapp_t), MALLOC_CAP_SPIRAM);
    int n = ext != NULL ? extapp_scan(ext, EXTAPP_MAX) : 0;
    cJSON *external = cJSON_AddArrayToObject(out, "external");
    for (int i = 0; i < n; i++)
    {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "id", ext[i].id);
        cJSON_AddStringToObject(j, "name", ext[i].name);
        cJSON_AddStringToObject(j, "version", ext[i].version);
        cJSON_AddNumberToObject(j, "size", (double)ext[i].size);
        cJSON_AddBoolToObject(j, "slot", ext[i].in_slot);
        cJSON_AddBoolToObject(j, "ready", extapp_is_cached(&ext[i]));
        cJSON_AddItemToArray(external, j);
    }
    free(ext);
    const settings_t *s = settings_get();
    cJSON *fav = cJSON_AddArrayToObject(out, "favorites");
    for (int i = 0; i < 6; i++)
    {
        if (s->favorites[i][0] != '\0')
        {
            cJSON_AddItemToArray(fav, cJSON_CreateString(s->favorites[i]));
        }
    }
    cJSON_AddNumberToObject(out, "cards", s->launcher_cards);
    return send_out(req, out);
}

static esp_err_t apps_open(httpd_req_t *req)
{
    cJSON *json = body(req);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(json, "id"));
    const char *ext_id = cJSON_GetStringValue(cJSON_GetObjectItem(json, "ext"));
    bool ok = false;
    if (id != NULL && app_find(id) != NULL)
    {
        lv_port_lock();
        app_close_others("web");   /* one app at a time from the browser (the Web page app stays) */
        ok = app_open(id, NULL);
        lv_port_unlock();
    }
    else if (ext_id != NULL)
    {
        extapp_t *list = heap_caps_calloc(EXTAPP_MAX, sizeof(extapp_t), MALLOC_CAP_SPIRAM);
        int n = list != NULL ? extapp_scan(list, EXTAPP_MAX) : 0;
        for (int i = 0; i < n && !ok; i++)
        {
            if (strcmp(list[i].id, ext_id) == 0)
            {
                extapp_t *copy = malloc(sizeof(extapp_t));   /* owned by the run screen */
                if (copy != NULL)
                {
                    *copy = list[i];
                    lv_port_lock();
                    app_close_others("web");
                    ok = app_open("extapps.open", copy);
                    lv_port_unlock();
                    if (!ok)
                    {
                        free(copy);
                    }
                }
            }
        }
        free(list);
    }
    cJSON_Delete(json);
    return reply(req, ok, ok ? "Aperta sull'orologio" : "App non trovata");
}

static esp_err_t apps_launcher(httpd_req_t *req)
{
    cJSON *json = body(req);
    if (json == NULL)
    {
        return reply(req, false, "Dati non validi");
    }
    settings_t s = *settings_get();
    const cJSON *fav = cJSON_GetObjectItem(json, "favorites");
    if (cJSON_IsArray(fav))
    {
        memset(s.favorites, 0, sizeof(s.favorites));
        int n = 0;
        const cJSON *item;
        cJSON_ArrayForEach(item, fav)
        {
            if (cJSON_IsString(item) && n < 6)
            {
                snprintf(s.favorites[n++], sizeof(s.favorites[0]), "%.23s", item->valuestring);
            }
        }
    }
    const cJSON *cards = cJSON_GetObjectItem(json, "cards");
    if (cJSON_IsNumber(cards) && (cards->valueint == 4 || cards->valueint == 6 || cards->valueint == 9))
    {
        s.launcher_cards = (uint8_t)cards->valueint;
    }
    cJSON_Delete(json);
    settings_save(&s);
    return reply(req, true, "Salvato: il menu si aggiorna alla prossima apertura");
}

/* ------------------------------------------------------------- market */

static esp_err_t market_get(httpd_req_t *req)
{
    market_status_t st;
    market_status(&st);
    cJSON *out = cJSON_CreateObject();
    cJSON *status = cJSON_AddObjectToObject(out, "status");
    cJSON_AddNumberToObject(status, "phase", st.phase);
    cJSON_AddNumberToObject(status, "percent", st.percent);
    cJSON_AddBoolToObject(status, "busy", st.busy);
    cJSON_AddStringToObject(status, "app", st.app_id);
    cJSON_AddStringToObject(status, "message", st.message);
    cJSON_AddNumberToObject(status, "revision", st.revision);
    cJSON_AddBoolToObject(status, "update_all", atomic_load(&s_update_all));
    cJSON_AddBoolToObject(out, "sd", sdcard_is_mounted());
    cJSON *list = cJSON_AddArrayToObject(out, "apps");
    market_entry_t *e = heap_caps_malloc(sizeof(*e), MALLOC_CAP_SPIRAM);
    for (int i = 0; e != NULL && market_entry(i, e); i++)
    {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "id", e->id);
        cJSON_AddStringToObject(j, "name", e->name);
        cJSON_AddStringToObject(j, "summary", e->summary);
        cJSON_AddStringToObject(j, "version", e->version);
        cJSON_AddStringToObject(j, "channel", e->channel);
        cJSON_AddNumberToObject(j, "color", e->color);
        cJSON_AddNumberToObject(j, "size", (double)e->binary_size);
        cJSON_AddStringToObject(j, "icon", e->icon_url);
        cJSON_AddBoolToObject(j, "installed", e->installed);
        cJSON_AddStringToObject(j, "installed_version", e->installed_version);
        cJSON_AddBoolToObject(j, "update", e->installed && e->installed_version[0] != '\0' &&
                                           fw_version_compare(e->version, e->installed_version) > 0);
        cJSON_AddItemToArray(list, j);
    }
    free(e);
    return send_out(req, out);
}

static esp_err_t market_action(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (strcmp(uri, "/api/market/refresh") == 0)
    {
        esp_err_t err = market_refresh();
        return reply(req, err == ESP_OK, err == ESP_OK ? "Carico il catalogo" : "Il Market è occupato");
    }
    cJSON *json = body(req);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(json, "id"));
    char copy[32] = "";
    snprintf(copy, sizeof(copy), "%s", id != NULL ? id : "");
    cJSON_Delete(json);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (copy[0] != '\0' && strcmp(uri, "/api/market/install") == 0)
    {
        err = market_install(copy);
    }
    else if (copy[0] != '\0' && strcmp(uri, "/api/market/remove") == 0)
    {
        err = market_remove(copy);
    }
    return reply(req, err == ESP_OK, err == ESP_OK ? "Avviato" : err == ESP_ERR_INVALID_STATE ? "Il Market è occupato"
                                                                                       : "App non trovata");
}

static void update_all_task(void *arg)
{
    (void)arg;
    market_entry_t *e = heap_caps_malloc(sizeof(*e), MALLOC_CAP_SPIRAM);
    char (*ids)[32] = heap_caps_calloc(MARKET_MAX_APPS, 32, MALLOC_CAP_SPIRAM);
    int n = 0;
    for (int i = 0; e != NULL && ids != NULL && market_entry(i, e) && n < MARKET_MAX_APPS; i++)
    {
        if (e->installed && e->installed_version[0] != '\0' && fw_version_compare(e->version, e->installed_version) > 0)
        {
            snprintf(ids[n++], 32, "%s", e->id);
        }
    }
    for (int i = 0; i < n; i++)
    {
        while (market_busy())
        {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (market_install(ids[i]) == ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(500));
            while (market_busy())
            {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    }
    free(e);
    free(ids);
    atomic_store(&s_update_all, false);
    vTaskDelete(NULL);
}

static esp_err_t market_update_all(httpd_req_t *req)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_update_all, &expected, true))
    {
        return reply(req, false, "Aggiornamento già in corso");
    }
    if (!sys_task_create(update_all_task, "update_all", 4096, NULL, 3, SYS_CORE_ANY))
    {
        atomic_store(&s_update_all, false);
        return reply(req, false, "Memoria insufficiente");
    }
    return reply(req, true, "Aggiorno tutte le app, una alla volta");
}

/* ------------------------------------------------------ system update */

static void check_task(void *arg)
{
    fw_update_check_online(arg != NULL);
    vTaskDelete(NULL);
}

static esp_err_t update_get(httpd_req_t *req)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "phase", fw_update_phase());
    cJSON_AddNumberToObject(out, "percent", fw_update_percent());
    cJSON_AddStringToObject(out, "message", fw_update_message() ? fw_update_message() : "");
    cJSON_AddStringToObject(out, "running", fw_update_running_version());
    cJSON_AddStringToObject(out, "available", fw_update_new_version() ? fw_update_new_version() : "");
    cJSON_AddBoolToObject(out, "is_available", fw_update_phase() == FW_UPDATE_AVAILABLE);
    return send_out(req, out);
}

static esp_err_t update_post(httpd_req_t *req)
{
    bool install = strcmp(req->uri, "/api/update/install") == 0;
    bool ok = sys_task_create(check_task, "fw_check", 6144, install ? (void *)1 : NULL, 3, SYS_CORE_ANY);
    return reply(req, ok, install ? "Installo l'aggiornamento..." : "Controllo...");
}

void web_system_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/system", .method = HTTP_GET, .handler = system_get},
        {.uri = "/api/kit", .method = HTTP_GET, .handler = system_get},   /* kit.js header */
        {.uri = "/api/kit/off", .method = HTTP_POST, .handler = kit_off},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post},
        {.uri = "/api/apps", .method = HTTP_GET, .handler = apps_get},
        {.uri = "/api/apps/open", .method = HTTP_POST, .handler = apps_open},
        {.uri = "/api/apps/launcher", .method = HTTP_POST, .handler = apps_launcher},
        {.uri = "/api/market", .method = HTTP_GET, .handler = market_get},
        {.uri = "/api/market/refresh", .method = HTTP_POST, .handler = market_action},
        {.uri = "/api/market/install", .method = HTTP_POST, .handler = market_action},
        {.uri = "/api/market/remove", .method = HTTP_POST, .handler = market_action},
        {.uri = "/api/market/update_all", .method = HTTP_POST, .handler = market_update_all},
        {.uri = "/api/update", .method = HTTP_GET, .handler = update_get},
        {.uri = "/api/update/check", .method = HTTP_POST, .handler = update_post},
        {.uri = "/api/update/install", .method = HTTP_POST, .handler = update_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
    {
        web_route(server, &routes[i]);
    }
}
