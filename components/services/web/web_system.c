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
#include "core/recovery.h"
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
#include "services/battery_health.h"
#include "services/motion.h"
#include "services/weather.h"
#if CONFIG_WATCH_ECOS_PAIRING
#include "services/ecos.h"
#endif

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "core/power.h"

#include <stdatomic.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_bool s_update_all;
static bool remote_ui_pending(void);
bool web_server_ui_pending(void) { return remote_ui_pending(); }

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
    if (storage_files_busy()) return httpd_resp_set_status(req, "503 Service Unavailable"), httpd_resp_sendstr(req, "Archiviazione occupata");
    const board_info_t *b = board_info();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "product", b->product);
    cJSON_AddStringToObject(out, "board", b->name);
    cJSON_AddStringToObject(out, "name", sys_nickname());
    cJSON_AddBoolToObject(out, "stop", !recovery_active());   /* kit.js: power button */
    char host[24];
    snprintf(host, sizeof(host), "%s.local", sys_hostname());
    cJSON_AddStringToObject(out, "hostname", host);
    cJSON_AddBoolToObject(out, "recovery", recovery_active());
    cJSON_AddBoolToObject(out, "recovery_supported", recovery_supported());
    cJSON *control = cJSON_AddObjectToObject(out, "control");
    #if CONFIG_WATCH_REMOTE_UI
    cJSON_AddBoolToObject(control, "remote_screen", true);
#else
    cJSON_AddBoolToObject(control, "remote_screen", false);
#endif
    cJSON_AddBoolToObject(control, "remote_screen_enabled", settings_get()->remote_ui_enabled);
    cJSON_AddBoolToObject(control, "physical_display", !lv_port_headless());
    cJSON_AddBoolToObject(control, "persistent_recovery_access", recovery_active());
    cJSON_AddBoolToObject(control, "recovery_network_watchdog", recovery_active());
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
    if (remote_ui_pending()) return reply(req, false, "Interfaccia occupata");
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
    if (storage_files_busy()) return reply(req, false, "Archiviazione occupata");
    esp_err_t err = reply(req, true, "Riavvio...");
    sys_post(restart_job, NULL);
    return err;
}

/* --------------------------------------------------------------- apps */

static esp_err_t apps_get(httpd_req_t *req)
{
    if (storage_files_busy()) return reply(req, false, "Archiviazione occupata");
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
        const char *view = NULL;
        if (strcmp(a->id, "sdcard") == 0) view = "v-storage";
        else if (strcmp(a->id, "alarms") == 0) view = "v-alarms";
        else if (strcmp(a->id, "ecos") == 0) view = "v-ecos";
        else if (strcmp(a->id, "settings") == 0) view = "v-settings";
        else if (strcmp(a->id, "market") == 0) view = "v-market";
        else if (strcmp(a->id, "battery") == 0) view = "v-battery";
        else if (strcmp(a->id, "sensors") == 0) view = "v-sensors";
        else if (strcmp(a->id, "weather") == 0) view = "v-weather";
        if (view != NULL && !cJSON_HasObjectItem(j, "dashboard"))
        {
            char url[40]; snprintf(url, sizeof(url), "/#%s", view);
            cJSON_AddStringToObject(j, "dashboard", url);
        }
        cJSON_AddStringToObject(j, "interaction", cJSON_HasObjectItem(j, "dashboard") ? "page" : "screen");
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
    if (remote_ui_pending()) return reply(req, false, "Interfaccia occupata, usa le funzioni dirette");
    if (storage_files_busy()) return reply(req, false, "Archiviazione occupata");
    cJSON *json = body(req);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(json, "id"));
    const char *ext_id = cJSON_GetStringValue(cJSON_GetObjectItem(json, "ext"));
    if (recovery_active() && (ext_id != NULL || (id != NULL && strncmp(id, "extapps.", 8) == 0)))
    {
        cJSON_Delete(json);
        return reply(req, false, "Avvio app esterne sospeso in recovery: richiede un riavvio fuori dal server");
    }
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

#if CONFIG_WATCH_ECOS_PAIRING
static esp_err_t ecos_get(httpd_req_t *req)
{
    ecos_snapshot_t s;
    ecos_snapshot(&s);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "enabled", true);
    cJSON_AddBoolToObject(out, "running", s.running);
    cJSON_AddBoolToObject(out, "comparing", s.comparing);
    cJSON_AddBoolToObject(out, "local_ok", s.local_ok);
    cJSON_AddBoolToObject(out, "remote_ok", s.remote_ok);
    cJSON_AddBoolToObject(out, "saved", s.saved);
    cJSON_AddNumberToObject(out, "seconds_left", s.seconds_left);
    cJSON_AddNumberToObject(out, "code", s.code);
    cJSON_AddStringToObject(out, "candidate", s.candidate);
    cJSON_AddStringToObject(out, "candidate_id", s.candidate_id);
    cJSON_AddStringToObject(out, "message", s.message);
    cJSON *peers = cJSON_AddArrayToObject(out, "peers");
    for (int i = 0; i < s.count; i++)
    {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", s.peers[i].name);
        cJSON_AddStringToObject(p, "id", s.peers[i].id);
        cJSON_AddItemToArray(peers, p);
    }
    return send_out(req, out);
}

static esp_err_t ecos_post(httpd_req_t *req)
{
    cJSON *json = body(req);
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "action"));
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (action != NULL && strcmp(action, "start") == 0)
    {
        cJSON *ip = cJSON_GetObjectItemCaseSensitive(json, "ip");
        if (ip == NULL || cJSON_IsString(ip))
        {
            err = ecos_pair_start(cJSON_GetStringValue(ip));
        }
    }
    else if (action != NULL && strcmp(action, "cancel") == 0)
    {
        ecos_pair_cancel();
        err = ESP_OK;
    }
    else if (action != NULL && strcmp(action, "confirm") == 0)
    {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "id"));
        cJSON *code = cJSON_GetObjectItemCaseSensitive(json, "code");
        if (id != NULL && strlen(id) == 12 && cJSON_IsNumber(code) &&
            code->valuedouble >= 0 && code->valuedouble <= 999999 && code->valuedouble == code->valueint)
        {
            err = ecos_pair_confirm(id, (uint32_t)code->valueint);
        }
    }
    cJSON_Delete(json);
    return reply(req, err == ESP_OK, err == ESP_OK ? "Richiesta ricevuta" :
                 err == ESP_ERR_NO_MEM ? "Memoria insufficiente" : "Richiesta non valida o sessione cambiata");
}

/* Famiglia: looking at it keeps the radio on for 90 s (each poll renews it). */
static esp_err_t family_get(httpd_req_t *req)
{
    ecos_family_touch();
    ecos_family_t *f = heap_caps_malloc(sizeof(*f), MALLOC_CAP_SPIRAM);
    if (f == NULL)
    {
        return reply(req, false, "Memoria insufficiente");
    }
    ecos_family(f);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "radio", f->radio);
    cJSON_AddBoolToObject(out, "busy", f->busy);
    cJSON_AddBoolToObject(out, "reachable_here", f->reachable_here);
    cJSON_AddStringToObject(out, "message", f->message);
    cJSON *members = cJSON_AddArrayToObject(out, "members");
    for (int i = 0; i < f->count; i++)
    {
        const ecos_member_t *m = &f->members[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "id", m->id);
        cJSON_AddStringToObject(j, "name", m->name);
        cJSON_AddBoolToObject(j, "known", m->known);
        cJSON_AddBoolToObject(j, "reachable", m->reachable);
        cJSON_AddNumberToObject(j, "permissions", m->permissions);
        cJSON_AddBoolToObject(j, "access_pending", m->access_pending);
        const char *hint = !m->reachable ? "Cerca il membro o apri Ecosistema sul dispositivo" :
            m->recovery ? "Recovery: usa pagine dedicate; schermo remoto solo se necessario" :
            !m->wifi ? "Radio Family disponibile; serve Wi-Fi per usare le pagine del membro" :
            m->battery >= 0 && m->battery < 15 && !m->usb ? "Batteria bassa: preferisci letture su richiesta" :
            "Richiedi le risorse per conoscere i servizi disponibili";
        cJSON_AddStringToObject(j, "suggestion", hint);
        if (m->access_known)
        {
            cJSON *access = cJSON_AddObjectToObject(j, "access");
            cJSON_AddNumberToObject(access, "operation", m->access_operation);
            cJSON_AddNumberToObject(access, "result", m->access_result);
            cJSON_AddNumberToObject(access, "age_s", m->access_age_s);
            if (m->access_result == 0)
            {
                cJSON_AddBoolToObject(access, "recovery", (m->resource_flags & 1) != 0);
                if (m->access_operation == 1)
                {
                    cJSON_AddStringToObject(access, "code", m->web_code);
                }
                else
                {
                    cJSON_AddNumberToObject(access, "battery_mv", m->battery_mv);
                    cJSON_AddNumberToObject(access, "battery", m->resource_battery);
                    cJSON_AddNumberToObject(access, "ram_kb", m->resource_ram_kb);
                    cJSON_AddBoolToObject(access, "sd_slot", (m->resource_flags & 64) != 0);
                    cJSON_AddBoolToObject(access, "web_available", (m->resource_flags & 128) != 0);
                    cJSON_AddBoolToObject(access, "imu_supported", (m->resource_flags & 2) != 0);
                    uint64_t age = (uint64_t)m->sample_age_ms + (uint64_t)m->access_age_s * 1000;
                    bool valid = (m->resource_flags & 4) != 0 && age <= 1000;
                    cJSON_AddBoolToObject(access, "imu_valid", valid);
                    if (valid)
                    {
                        cJSON *acceleration = cJSON_AddArrayToObject(access, "acceleration_mg");
                        cJSON *angular = cJSON_AddArrayToObject(access, "angular_dps");
                        for (int axis = 0; axis < 3; axis++)
                        {
                            cJSON_AddItemToArray(acceleration, cJSON_CreateNumber(m->acceleration_mg[axis]));
                            cJSON_AddItemToArray(angular, cJSON_CreateNumber(m->angular_dps[axis]));
                        }
                        cJSON_AddNumberToObject(access, "sample_age_ms", (double)age);
                    }
                }
            }
        }
        cJSON_AddNumberToObject(j, "seen_ago", m->seen_ago == UINT32_MAX ? -1 : (double)m->seen_ago);
        if (m->known)
        {
            cJSON_AddNumberToObject(j, "rssi", m->rssi);
            cJSON_AddNumberToObject(j, "channel", m->channel);
            cJSON_AddStringToObject(j, "product", m->product);
            cJSON_AddStringToObject(j, "version", m->version);
            cJSON_AddNumberToObject(j, "battery", m->battery);
            cJSON_AddBoolToObject(j, "recovery", m->recovery);
            cJSON_AddBoolToObject(j, "charging", m->charging);
            cJSON_AddBoolToObject(j, "usb", m->usb);
            cJSON_AddBoolToObject(j, "listening", m->listening);
            cJSON_AddBoolToObject(j, "ringing", m->ringing);
            cJSON_AddStringToObject(j, "ssid", m->wifi ? m->ssid : "");
            cJSON_AddStringToObject(j, "ip", m->wifi ? m->ip : "");
            cJSON_AddNumberToObject(j, "uptime_min", m->uptime_min);
            cJSON_AddNumberToObject(j, "ram_kb", m->ram_kb);
        }
        cJSON_AddItemToArray(members, j);
    }
    free(f);
    return send_out(req, out);
}

static esp_err_t family_post(httpd_req_t *req)
{
    cJSON *json = body(req);
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "action"));
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "id"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "text"));
    esp_err_t err = ESP_ERR_INVALID_ARG;
    ecos_family_touch();
    if (action == NULL)
    {
    }
    else if (strcmp(action, "refresh") == 0)
    {
        err = ecos_family_refresh();
    }
    else if (strcmp(action, "ring") == 0 || strcmp(action, "stop") == 0)
    {
        err = ecos_family_ring(id, strcmp(action, "ring") == 0);
    }
    else if (strcmp(action, "message") == 0 && text != NULL && strlen(text) < 96)
    {
        err = ecos_family_message(id, text);
    }
    else if (strcmp(action, "web_code") == 0 || strcmp(action, "resources") == 0)
    {
        err = ecos_family_request(id, strcmp(action, "web_code") == 0 ? 1 : 2);
    }
    else if (strcmp(action, "permissions") == 0)
    {
        const cJSON *permissions = cJSON_GetObjectItemCaseSensitive(json, "permissions");
        if (cJSON_IsNumber(permissions) && permissions->valuedouble >= 0 &&
            permissions->valuedouble <= 3 && permissions->valuedouble == permissions->valueint)
        {
            err = ecos_permissions_set(id, (uint32_t)permissions->valueint);
        }
    }
    else if (strcmp(action, "forget") == 0)
    {
        err = ecos_forget(id);
    }
    cJSON_Delete(json);
    return reply(req, err == ESP_OK, err == ESP_OK ? "Richiesta ricevuta" :
                 err == ESP_ERR_INVALID_STATE ? "Attendi: un'altra operazione è in corso" : "Richiesta non valida");
}
#else
static esp_err_t ecos_get(httpd_req_t *req)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "enabled", false);
    return send_out(req, out);
}
#endif


/* Remote UI: snapshots and navigation run in the LVGL task, never HTTP. */
typedef struct
{
    SemaphoreHandle_t done;
    lv_draw_buf_t *screen;
    lv_draw_buf_t *top;
    int action;
} remote_call_t;

/* A delayed UI callback must never keep the HTTP task or a caller stack alive.
 * The UI and HTTP each own a reference; at most one pending UI job is retained. */
typedef struct
{
    remote_call_t call;
    atomic_int references;
    atomic_bool cancelled;
} remote_shared_t;
static atomic_bool s_remote_pending;
static bool remote_ui_pending(void) { return atomic_load(&s_remote_pending); }
static void remote_destroy(void *arg)
{
    remote_shared_t *shared = arg;
    if (shared->call.screen != NULL) lv_draw_buf_destroy(shared->call.screen);
    if (shared->call.top != NULL) lv_draw_buf_destroy(shared->call.top);
    vSemaphoreDelete(shared->call.done);
    free(shared);
    atomic_store(&s_remote_pending, false);
}
static void remote_release(remote_shared_t *shared, bool in_ui)
{
    if (atomic_fetch_sub(&shared->references, 1) != 1) return;
    if (in_ui || (shared->call.screen == NULL && shared->call.top == NULL)) remote_destroy(shared);
    else if (!ui_async(remote_destroy, shared))
    {
        /* Keep this single allocation, never free LVGL buffers from HTTP.
         * The pending guard rejects further captures instead of growing a queue. */
    }
}
static void remote_ui(void *arg)
{
    remote_shared_t *shared = arg;
    remote_call_t *call = &shared->call;
    if (!atomic_load(&shared->cancelled) || call->action == 1)
    {
        if (call->action == 0)
        {
#if CONFIG_WATCH_REMOTE_UI
            call->screen = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
            if (lv_obj_get_child_count(lv_layer_top()) > 0)
                call->top = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
#endif
        }
        else if (call->action == 1)
        {
            if (call->screen != NULL) lv_draw_buf_destroy(call->screen);
            if (call->top != NULL) lv_draw_buf_destroy(call->top);
            call->screen = call->top = NULL;
        }
        else if (call->action == 2) app_back_user();
        else if (call->action == 3) app_home();
        else if (call->action == 4) app_close_others("web");
    }
    xSemaphoreGive(call->done);
    remote_release(shared, true);
}
static bool remote_run(remote_call_t *call)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_remote_pending, &expected, true)) return false;
    remote_shared_t *shared = calloc(1, sizeof(*shared));
    if (shared == NULL) { atomic_store(&s_remote_pending, false); return false; }
    shared->call = *call;
    shared->call.done = xSemaphoreCreateBinary();
    if (shared->call.done == NULL)
    {
        free(shared); atomic_store(&s_remote_pending, false); return false;
    }
    atomic_init(&shared->references, 2);
    atomic_init(&shared->cancelled, false);
    if (!ui_async(remote_ui, shared))
    {
        /* The job was never accepted; caller still owns its buffers. */
        shared->call.screen = shared->call.top = NULL;
        remote_release(shared, true); remote_release(shared, true);
        return false;
    }
    bool ready = xSemaphoreTake(shared->call.done, pdMS_TO_TICKS(3000)) == pdTRUE;
    if (ready)
    {
        SemaphoreHandle_t original = call->done;
        *call = shared->call; call->done = original;
        shared->call.screen = shared->call.top = NULL; /* ownership returned to HTTP */
    }
    else atomic_store(&shared->cancelled, true);
    remote_release(shared, false);
    return ready;
}

#if CONFIG_WATCH_REMOTE_UI
static void remote_u32(uint8_t *p, uint32_t value)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (i * 8));
}

static esp_err_t remote_frame(httpd_req_t *req)
{
    if (!settings_get()->remote_ui_enabled) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Schermo remoto disattivato");
    remote_call_t call = {.done = xSemaphoreCreateBinary()};
    if (call.done == NULL) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memoria insufficiente");
    power_wake(POWER_WAKE_OTHER);
    bool ready = remote_run(&call) && call.screen != NULL;
    esp_err_t err = ESP_FAIL;
    if (ready)
    {
        unsigned w = call.screen->header.w, h = call.screen->header.h;
        unsigned stride = (w * 3 + 3) & ~3U;
        uint8_t *row = heap_caps_calloc(1, stride, MALLOC_CAP_SPIRAM);
        if (row != NULL)
        {
            uint8_t header[54] = {'B', 'M'};
            remote_u32(header + 2, 54 + stride * h);
            remote_u32(header + 10, 54);
            remote_u32(header + 14, 40);
            remote_u32(header + 18, w);
            remote_u32(header + 22, (uint32_t)-(int32_t)h);
            header[26] = 1;
            header[28] = 24;
            httpd_resp_set_type(req, "image/bmp");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            err = httpd_resp_send_chunk(req, (char *)header, sizeof(header));
            for (unsigned y = 0; y < h && err == ESP_OK; y++)
            {
                const uint16_t *src = (const uint16_t *)(call.screen->data + y * call.screen->header.stride);
                const uint8_t *top = call.top != NULL && y < call.top->header.h ?
                                     call.top->data + y * call.top->header.stride : NULL;
                for (unsigned x = 0; x < w; x++)
                {
                    unsigned b = (src[x] & 31) * 255 / 31;
                    unsigned g = ((src[x] >> 5) & 63) * 255 / 63;
                    unsigned r = ((src[x] >> 11) & 31) * 255 / 31;
                    if (top != NULL && x < call.top->header.w)
                    {
                        unsigned a = top[x * 4 + 3];
                        b = (top[x * 4] * a + b * (255 - a)) / 255;
                        g = (top[x * 4 + 1] * a + g * (255 - a)) / 255;
                        r = (top[x * 4 + 2] * a + r * (255 - a)) / 255;
                    }
                    row[x * 3] = b; row[x * 3 + 1] = g; row[x * 3 + 2] = r;
                }
                err = httpd_resp_send_chunk(req, (char *)row, stride);
            }
            free(row);
            if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
        }
        else err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memoria insufficiente");
    }
    else err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Schermo non disponibile");
    call.action = 1;
    if (call.screen != NULL || call.top != NULL) remote_run(&call);
    vSemaphoreDelete(call.done);
    return err;
}

typedef struct
{
    int x, y, end_x, end_y;
    int step, steps;
} remote_gesture_t;

static atomic_bool s_remote_gesture;

/* Timed input belongs to the UI task; the HTTP server must never wait through
 * a gesture, which would delay every other dashboard and recovery request. */
static void remote_gesture_tick(lv_timer_t *timer)
{
    remote_gesture_t *gesture = lv_timer_get_user_data(timer);
    if (gesture->step >= gesture->steps)
    {
        lv_port_fake_touch(false, gesture->end_x, gesture->end_y);
        lv_timer_delete(timer);
        free(gesture);
        atomic_store(&s_remote_gesture, false);
        return;
    }
    gesture->step++;
    int x = gesture->x + (gesture->end_x - gesture->x) * gesture->step / gesture->steps;
    int y = gesture->y + (gesture->end_y - gesture->y) * gesture->step / gesture->steps;
    lv_port_fake_touch(true, x, y);
}

static void remote_gesture_start(void *arg)
{
    remote_gesture_t *gesture = arg;
    lv_timer_t *timer = lv_timer_create(remote_gesture_tick, 30, gesture);
    if (timer == NULL)
    {
        free(gesture);
        atomic_store(&s_remote_gesture, false);
        return;
    }
    lv_port_fake_touch(true, gesture->x, gesture->y);
}

static esp_err_t remote_input(httpd_req_t *req)
{
    if (!settings_get()->remote_ui_enabled) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Schermo remoto disattivato");
    cJSON *json = body(req);
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(json, "action");
    bool ok = false;
    if (cJSON_IsString(kind) && (strcmp(kind->valuestring, "back") == 0 || strcmp(kind->valuestring, "home") == 0))
    {
        remote_call_t call = {.done = xSemaphoreCreateBinary(), .action = strcmp(kind->valuestring, "back") == 0 ? 2 : 3};
        if (call.done != NULL)
        {
            power_wake(POWER_WAKE_OTHER);
            ok = remote_run(&call);
            vSemaphoreDelete(call.done);
        }
    }
    else if (cJSON_IsString(kind) && (strcmp(kind->valuestring, "tap") == 0 || strcmp(kind->valuestring, "swipe") == 0))
    {
        const cJSON *x = cJSON_GetObjectItemCaseSensitive(json, "x"), *y = cJSON_GetObjectItemCaseSensitive(json, "y");
        const cJSON *ex = cJSON_GetObjectItemCaseSensitive(json, "end_x"), *ey = cJSON_GetObjectItemCaseSensitive(json, "end_y");
        bool swipe = strcmp(kind->valuestring, "swipe") == 0;
        int w = board_info()->width, h = board_info()->height;
        ok = cJSON_IsNumber(x) && cJSON_IsNumber(y) && x->valuedouble >= 0 && x->valuedouble < w &&
             y->valuedouble >= 0 && y->valuedouble < h;
        if (swipe) ok = ok && cJSON_IsNumber(ex) && cJSON_IsNumber(ey) && ex->valuedouble >= 0 &&
                        ex->valuedouble < w && ey->valuedouble >= 0 && ey->valuedouble < h;
        if (ok)
        {
            bool expected = false;
            ok = atomic_compare_exchange_strong(&s_remote_gesture, &expected, true);
            if (ok)
            {
                remote_gesture_t *gesture = calloc(1, sizeof(*gesture));
                if (gesture != NULL)
                {
                    gesture->x = x->valueint;
                    gesture->y = y->valueint;
                    gesture->end_x = swipe ? ex->valueint : x->valueint;
                    gesture->end_y = swipe ? ey->valueint : y->valueint;
                    gesture->steps = swipe ? 10 : 2;
                    power_wake(POWER_WAKE_OTHER);
                    ok = ui_async(remote_gesture_start, gesture);
                    if (!ok) free(gesture);
                }
                else ok = false;
                if (!ok) atomic_store(&s_remote_gesture, false);
            }
        }
    }
    cJSON_Delete(json);
    return reply(req, ok, ok ? "Comando eseguito" : "Comando non valido");
}

#endif

/* Device services are independent of LVGL snapshots. */
static esp_err_t remote_config_get(httpd_req_t *req)
{
    cJSON *out = cJSON_CreateObject();
#if CONFIG_WATCH_REMOTE_UI
    cJSON_AddBoolToObject(out, "supported", true);
#else
    cJSON_AddBoolToObject(out, "supported", false);
#endif
    cJSON_AddBoolToObject(out, "enabled", settings_get()->remote_ui_enabled);
    cJSON_AddNumberToObject(out, "retained_frame_bytes", remote_ui_pending() ? -1 : 0);
    cJSON_AddBoolToObject(out, "ui_pending", remote_ui_pending());
    return send_out(req, out);
}

static esp_err_t remote_config_post(httpd_req_t *req)
{
    cJSON *json = body(req);
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    bool ok = cJSON_IsBool(enabled);
#if !CONFIG_WATCH_REMOTE_UI
    if (cJSON_IsTrue(enabled)) ok = false;
#endif
    if (ok)
    {
        settings_t s = *settings_get(); s.remote_ui_enabled = cJSON_IsTrue(enabled);
        settings_save(&s);
        settings_flush(); /* Explicit switch survives an immediate reboot. */
    }
    cJSON_Delete(json);
    return reply(req, ok, ok ? "Preferenza salvata" : "Richiesta non valida");
}

/* One asynchronous SD job; the global file lease excludes transfers/USB handover. */
static atomic_int s_disk_job, s_disk_error;
static atomic_uint s_disk_generation;
bool web_storage_maintenance(void)
{
    int job = atomic_load(&s_disk_job);
    return job == 1 || job == 2;
}
static void disk_worker(void *arg)
{
    int operation = (int)(intptr_t)arg;
    esp_err_t err = ESP_FAIL;
    remote_call_t close = {.action = 4};
    if (remote_run(&close))
    {
        atomic_store(&s_disk_job, 2);
        err = operation == 1 ? sdcard_format() : sdcard_mount();
        if (err == ESP_OK && operation == 1)
        {
            const char *paths[] = {SDCARD_MOUNT "/apps", SDCARD_MOUNT "/config"};
            for (unsigned i = 0; i < 2; i++)
                if (mkdir(paths[i], 0775) != 0 && errno != EEXIST) err = ESP_FAIL;
        }
    }
    atomic_store(&s_disk_error, err);
    atomic_store(&s_disk_job, err == ESP_OK ? 3 : 4);
    storage_files_release();
    vTaskDelete(NULL);
}
static esp_err_t disk_get(httpd_req_t *req)
{
    cJSON *out = cJSON_CreateObject();
    int job = atomic_load(&s_disk_job);
    bool busy = job == 1 || job == 2;
    cJSON_AddBoolToObject(out, "supported", board_info()->has_sdcard);
    cJSON_AddBoolToObject(out, "busy", busy);
    cJSON_AddNumberToObject(out, "job", job);
    cJSON_AddNumberToObject(out, "generation", atomic_load(&s_disk_generation));
    cJSON_AddStringToObject(out, "error", job == 4 ? esp_err_to_name(atomic_load(&s_disk_error)) : "");
    /* Never touch FAT or SD registers while the worker formats. */
    if (!busy)
    {
        sdcard_state_t state = sdcard_state();
        cJSON_AddNumberToObject(out, "state", state);
        cJSON_AddBoolToObject(out, "can_format", board_info()->has_sdcard &&
            (state == SDCARD_MOUNTED || state == SDCARD_UNREADABLE) && !storage_files_busy());
        if (state == SDCARD_MOUNTED)
        {
            uint32_t total, free_mb; sdcard_space(&total, &free_mb);
            cJSON_AddNumberToObject(out, "total_mb", total);
            cJSON_AddNumberToObject(out, "free_mb", free_mb);
            sdcard_info_t info;
            if (sdcard_info(&info) == ESP_OK) cJSON_AddStringToObject(out, "name", info.name);
        }
    }
    return send_out(req, out);
}
static esp_err_t disk_post(httpd_req_t *req)
{
    cJSON *json = body(req);
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "action"));
    const char *confirmation = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "confirmation"));
    int operation = action != NULL && strcmp(action, "format") == 0 ? 1 :
                    action != NULL && strcmp(action, "mount") == 0 ? 2 : 0;
    bool confirmed = operation != 1 || (confirmation != NULL && strcmp(confirmation, "FORMAT SD") == 0);
    cJSON_Delete(json);
    if (remote_ui_pending()) return reply(req, false, "Interfaccia occupata: riprova dopo il riavvio");
    if (!operation || !confirmed || !board_info()->has_sdcard)
        return reply(req, false, "Operazione non supportata o conferma mancante");
    fw_update_phase_t update = fw_update_phase();
    if (update == FW_UPDATE_CHECKING || update == FW_UPDATE_WRITING || update == FW_UPDATE_RESTARTING)
        return reply(req, false, "Aggiornamento firmware in corso");
    if (!storage_files_acquire()) return reply(req, false, "Archiviazione occupata");
    sdcard_state_t state = sdcard_state();
    if (state == SDCARD_BUSY || (operation == 1 && state != SDCARD_MOUNTED && state != SDCARD_UNREADABLE))
    {
        storage_files_release();
        return reply(req, false, "Scheda assente o occupata da USB");
    }
    atomic_store(&s_disk_job, 1);
    atomic_fetch_add(&s_disk_generation, 1);
    if (!sys_task_create(disk_worker, "web_sd", 6144, (void *)(intptr_t)operation, 3, SYS_CORE_ANY))
    {
        atomic_store(&s_disk_error, ESP_ERR_NO_MEM); atomic_store(&s_disk_job, 4);
        storage_files_release(); return reply(req, false, "Memoria insufficiente");
    }
    return disk_get(req);
}

/* Cached service snapshots: no LVGL lock, capture buffers or sensor startup. */
static esp_err_t battery_get(httpd_req_t *req)
{
    battery_report_t r = {0};
    battery_health_get(&r);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "supported", pmu_present());
    cJSON_AddBoolToObject(out, "present", pmu_battery_present());
    cJSON_AddNumberToObject(out, "percent", r.percent);
    cJSON_AddNumberToObject(out, "mv", r.mv);
    cJSON_AddBoolToObject(out, "charging", r.charging);
    cJSON_AddBoolToObject(out, "usb", r.usb);
    cJSON_AddNumberToObject(out, "health_estimate", r.health);
    cJSON_AddNumberToObject(out, "cycles_x10", r.cycles_x10);
    cJSON_AddNumberToObject(out, "days", r.days);
    cJSON_AddNumberToObject(out, "minutes_left_estimate", r.minutes_left);
    return send_out(req, out);
}
static esp_err_t sensors_get(httpd_req_t *req)
{
    motion_snapshot_t r = {0};
    motion_snapshot(&r);
    int64_t age = r.sample_monotonic_us > 0 ? (esp_timer_get_time() - r.sample_monotonic_us) / 1000 : -1;
    bool fresh = r.valid && r.running && age >= 0 && age <= 1000;
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "supported", imu_supported());
    cJSON_AddBoolToObject(out, "running", r.running);
    cJSON_AddBoolToObject(out, "fresh", fresh);
    cJSON_AddNumberToObject(out, "age_ms", (double)age);
    cJSON_AddNumberToObject(out, "samples", r.samples);
    cJSON_AddStringToObject(out, "error", esp_err_to_name(r.error));
    if (fresh)
    {
        cJSON *accel = cJSON_AddArrayToObject(out, "acceleration_g");
        cJSON *gyro = cJSON_AddArrayToObject(out, "angular_dps");
        for (unsigned i = 0; i < 3; i++)
        {
            cJSON_AddItemToArray(accel, cJSON_CreateNumber(r.sample.acceleration_g[i]));
            cJSON_AddItemToArray(gyro, cJSON_CreateNumber(r.sample.angular_dps[i]));
        }
        cJSON_AddNumberToObject(out, "temperature_c", r.sample.temperature_c);
    }
    return send_out(req, out);
}
static esp_err_t weather_get_api(httpd_req_t *req)
{
    weather_t r = {0};
    bool valid = weather_get(&r) && r.valid;
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "valid", valid);
    cJSON_AddBoolToObject(out, "updating", weather_is_updating());
    cJSON_AddStringToObject(out, "city", r.city);
    if (valid)
    {
        cJSON_AddBoolToObject(out, "from_phone", r.from_phone);
        cJSON_AddNumberToObject(out, "updated", (double)r.updated);
        cJSON_AddNumberToObject(out, "temperature_c", r.temp);
        cJSON_AddNumberToObject(out, "min_c", r.temp_min);
        cJSON_AddNumberToObject(out, "max_c", r.temp_max);
        cJSON_AddNumberToObject(out, "wind_kmh", r.wind_kmh);
        cJSON_AddNumberToObject(out, "humidity", r.humidity);
        cJSON_AddNumberToObject(out, "code", r.code);
        cJSON *days = cJSON_AddArrayToObject(out, "days");
        for (int i = 0; i < r.day_count && i < WEATHER_DAYS; i++)
        {
            cJSON *day = cJSON_CreateObject();
            cJSON_AddNumberToObject(day, "code", r.days[i].code);
            cJSON_AddNumberToObject(day, "min_c", r.days[i].temp_min);
            cJSON_AddNumberToObject(day, "max_c", r.days[i].temp_max);
            cJSON_AddItemToArray(days, day);
        }
    }
    return send_out(req, out);
}

static esp_err_t capabilities_get(httpd_req_t *req)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "version", 1);
    cJSON *features = cJSON_AddArrayToObject(out, "features");
    const char *services[][3] = {
        {"settings", "/api/state", "/#v-settings"}, {"wifi", "/api/wifi/status", "/#v-wifi"},
        {"alarms", "/api/alarms", "/#v-alarms"}, {"files", "/api/fs/list?path=/", "/#v-files"},
        {"apps", "/api/apps", "/#v-apps"}, {"updates", "/api/update", "/#v-system"},
        {"family", "/api/ecos/family", "/#v-ecos"}, {"storage", "/api/storage", "/#v-storage"},
        {"remote_screen", "/api/remote", "/#v-remote"},
        {"battery", "/api/battery", "/#v-battery"}, {"sensors", "/api/sensors", "/#v-sensors"},
        {"weather", "/api/weather", "/#v-weather"}
    };
    for (unsigned i = 0; i < sizeof(services)/sizeof(services[0]); i++)
    {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "id", services[i][0]);
        cJSON_AddStringToObject(f, "api", services[i][1]);
        cJSON_AddStringToObject(f, "page", services[i][2]);
        bool available = true;
        if (i == 7) available = board_info()->has_sdcard;
        if (i == 9) available = pmu_present();
        if (i == 10) available = imu_supported();
#if !CONFIG_WATCH_ECOS_PAIRING
        if (i == 6) available = false;
#endif
#if !CONFIG_WATCH_REMOTE_UI
        if (i == 8) available = false;
#endif
        cJSON_AddBoolToObject(f, "available", available);
        cJSON_AddItemToArray(features, f);
    }
    return send_out(req, out);
}


static esp_err_t recovery_post(httpd_req_t *req)
{
    if (storage_files_busy()) return reply(req, false, "Archiviazione occupata");
    cJSON *json = body(req);
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    bool ok = cJSON_IsBool(enabled) && recovery_request(cJSON_IsTrue(enabled));
    cJSON_Delete(json);
    return reply(req, ok, ok ? "Riavvio richiesto" : "Richiesta non valida o gia in corso");
}

void web_system_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/battery", .method = HTTP_GET, .handler = battery_get},
        {.uri = "/api/sensors", .method = HTTP_GET, .handler = sensors_get},
        {.uri = "/api/weather", .method = HTTP_GET, .handler = weather_get_api},
        {.uri = "/api/capabilities", .method = HTTP_GET, .handler = capabilities_get},
        {.uri = "/api/storage", .method = HTTP_GET, .handler = disk_get},
        {.uri = "/api/storage", .method = HTTP_POST, .handler = disk_post},
        {.uri = "/api/remote", .method = HTTP_GET, .handler = remote_config_get},
        {.uri = "/api/remote", .method = HTTP_POST, .handler = remote_config_post},
#if CONFIG_WATCH_REMOTE_UI
        {.uri = "/api/remote/frame", .method = HTTP_GET, .handler = remote_frame},
        {.uri = "/api/remote/input", .method = HTTP_POST, .handler = remote_input},
#endif
        {.uri = "/api/ecos", .method = HTTP_GET, .handler = ecos_get},
#if CONFIG_WATCH_ECOS_PAIRING
        {.uri = "/api/ecos", .method = HTTP_POST, .handler = ecos_post},
        {.uri = "/api/ecos/family", .method = HTTP_GET, .handler = family_get},
        {.uri = "/api/ecos/family", .method = HTTP_POST, .handler = family_post},
#endif
        {.uri = "/api/system", .method = HTTP_GET, .handler = system_get},
        {.uri = "/api/kit", .method = HTTP_GET, .handler = system_get},   /* kit.js header */
        {.uri = "/api/kit/off", .method = HTTP_POST, .handler = kit_off},
        {.uri = "/api/recovery", .method = HTTP_POST, .handler = recovery_post},
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
