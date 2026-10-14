/*
 * Copertura dashboard API (served by appkit; every route needs the paired token).
 *
 *   GET  /api/state         live signal, measure, finder, rooms, floors, plans
 *   GET  /api/history       last two minutes: {rssi:[...], ping:[...]}
 *   GET  /api/points        every measured spot
 *   GET  /api/aps           last scan + channel advice
 *   GET  /api/plan?floor=0  floor plan image
 *   POST /api/plan?floor=0  upload a floor plan (JPEG/PNG, max 900 KB)
 *   POST /api/plan/delete?floor=0
 *   POST /api/measure       {room, floor}
 *   POST /api/point/place   {id, x, y, floor}   (x, y in thousandths of the plan)
 *   POST /api/point/delete  {id}
 *   POST /api/points/clear
 *   POST /api/rooms         {rooms:[...], floors:[...]}
 *   POST /api/scan
 *   POST /api/finder        {mode: 0|1, or -1 to stop}
 */
#include "copertura.h"

#include "appkit.h"
#include "services/storage.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern const char dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const char dashboard_html_end[] asm("_binary_dashboard_html_end");

#define GUARD() do { if (!appkit_authorized(request)) return appkit_unauthorized(request); } while (0)
#define PLAN_MAX (900 * 1024)

static double num(const cJSON *o, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static cJSON *body_json(httpd_req_t *request)
{
    char *raw = appkit_body(request, 4096);
    cJSON *root = raw != NULL ? cJSON_Parse(raw) : NULL;
    free(raw);
    return root;
}

static void mac_text(const uint8_t *b, char *out)
{
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
}

static esp_err_t state(httpd_req_t *request)
{
    GUARD();
    cp_live_t l;
    cp_live(&l);
    cp_measure_t m;
    cp_measure_status(&m);
    cp_finder_t f;
    cp_finder_state(&f);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "revision", cp_revision());
    cJSON *live = cJSON_AddObjectToObject(out, "live");
    char mac[18];
    mac_text(l.bssid, mac);
    cJSON_AddBoolToObject(live, "connected", l.connected);
    cJSON_AddStringToObject(live, "ssid", l.ssid);
    cJSON_AddStringToObject(live, "bssid", mac);
    cJSON_AddNumberToObject(live, "channel", l.channel);
    cJSON_AddNumberToObject(live, "rssi", l.rssi);
    cJSON_AddNumberToObject(live, "ping", l.ping);
    cJSON_AddNumberToObject(live, "ping_avg", l.ping_avg);
    cJSON_AddNumberToObject(live, "loss", l.loss);
    cJSON_AddStringToObject(live, "gateway", l.gateway);
    cJSON_AddStringToObject(live, "phy", l.phy);
    cJSON_AddNumberToObject(live, "grade", cp_grade(l.rssi, l.ping_avg, l.loss));
    cJSON *measure = cJSON_AddObjectToObject(out, "measure");
    cJSON_AddNumberToObject(measure, "phase", m.phase);
    cJSON_AddNumberToObject(measure, "percent", m.percent);
    cJSON_AddStringToObject(measure, "room", m.room);
    cJSON_AddNumberToObject(measure, "last", m.last_point);
    cJSON *finder = cJSON_AddObjectToObject(out, "finder");
    cJSON_AddBoolToObject(finder, "on", f.on);
    cJSON_AddNumberToObject(finder, "mode", f.mode);
    cJSON_AddNumberToObject(finder, "heat", f.heat);
    cJSON_AddNumberToObject(finder, "best", f.best);
    char list[CP_MAX_ROOMS][CP_ROOM];
    int n = cp_rooms(list, CP_MAX_ROOMS);
    cJSON *rooms = cJSON_AddArrayToObject(out, "rooms");
    for (int i = 0; i < n; i++)
    {
        cJSON_AddItemToArray(rooms, cJSON_CreateString(list[i]));
    }
    n = cp_floors(list, CP_MAX_FLOORS);
    cJSON *floors = cJSON_AddArrayToObject(out, "floors");
    cJSON *plans = cJSON_AddArrayToObject(out, "plans");
    for (int i = 0; i < n; i++)
    {
        cJSON_AddItemToArray(floors, cJSON_CreateString(list[i]));
        char path[80];
        struct stat st;
        cJSON_AddItemToArray(plans, cJSON_CreateBool(cp_plan_path(i, path, sizeof(path)) && stat(path, &st) == 0 && st.st_size > 0));
    }
    return appkit_json(request, out);
}

static esp_err_t history(httpd_req_t *request)
{
    GUARD();
    int8_t rssi[CP_LIVE];
    int16_t ping[CP_LIVE];
    int n = cp_history(rssi, ping, CP_LIVE);
    cJSON *out = cJSON_CreateObject();
    cJSON *r = cJSON_AddArrayToObject(out, "rssi");
    cJSON *p = cJSON_AddArrayToObject(out, "ping");
    for (int i = 0; i < n; i++)
    {
        cJSON_AddItemToArray(r, cJSON_CreateNumber(rssi[i]));
        cJSON_AddItemToArray(p, cJSON_CreateNumber(ping[i]));
    }
    return appkit_json(request, out);
}

static esp_err_t points(httpd_req_t *request)
{
    GUARD();
    cp_point_t *copy = heap_caps_malloc(sizeof(cp_point_t) * CP_MAX_POINTS, MALLOC_CAP_SPIRAM);
    if (copy == NULL)
    {
        return appkit_json(request, NULL);
    }
    cp_lock();
    int count = 0;
    const cp_point_t *list = cp_points(&count);
    memcpy(copy, list, sizeof(cp_point_t) * count);
    cp_unlock();
    cJSON *out = cJSON_CreateArray();
    for (int i = 0; i < count; i++)
    {
        const cp_point_t *p = &copy[i];
        cJSON *j = cJSON_CreateObject();
        char mac[18];
        mac_text(p->bssid, mac);
        cJSON_AddNumberToObject(j, "id", p->id);
        cJSON_AddStringToObject(j, "room", p->room);
        cJSON_AddNumberToObject(j, "floor", p->floor);
        cJSON_AddNumberToObject(j, "x", p->x);
        cJSON_AddNumberToObject(j, "y", p->y);
        cJSON_AddNumberToObject(j, "rssi", p->rssi);
        cJSON_AddNumberToObject(j, "min", p->rssi_min);
        cJSON_AddNumberToObject(j, "max", p->rssi_max);
        cJSON_AddNumberToObject(j, "channel", p->channel);
        cJSON_AddStringToObject(j, "bssid", mac);
        cJSON_AddNumberToObject(j, "ping", p->ping);
        cJSON_AddNumberToObject(j, "ping_max", p->ping_max);
        cJSON_AddNumberToObject(j, "loss", p->loss);
        cJSON_AddNumberToObject(j, "radios", p->radios);
        cJSON_AddNumberToObject(j, "alt", p->alt_rssi);
        cJSON_AddNumberToObject(j, "when", (double)p->when);
        cJSON_AddNumberToObject(j, "grade", cp_grade(p->rssi, p->ping, p->loss));
        cJSON_AddItemToArray(out, j);
    }
    free(copy);
    return appkit_json(request, out);
}

static esp_err_t aps(httpd_req_t *request)
{
    GUARD();
    cp_live_t l;
    cp_live(&l);
    cJSON *out = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(out, "aps");
    cp_neighbour_t *neighbours = heap_caps_malloc(sizeof(cp_neighbour_t) * CP_MAX_APS, MALLOC_CAP_SPIRAM);
    int m = 0;
    cp_lock();
    int count = 0;
    int64_t when = 0;
    const cp_ap_t *a = cp_aps(&count, &when);
    for (int i = 0; i < count; i++)
    {
        cJSON *j = cJSON_CreateObject();
        char mac[18];
        mac_text(a[i].bssid, mac);
        cJSON_AddStringToObject(j, "ssid", a[i].ssid);
        cJSON_AddStringToObject(j, "bssid", mac);
        cJSON_AddNumberToObject(j, "channel", a[i].channel);
        cJSON_AddNumberToObject(j, "rssi", a[i].rssi);
        cJSON_AddNumberToObject(j, "auth", a[i].auth);
        cJSON_AddBoolToObject(j, "ours", a[i].ours);
        cJSON_AddItemToArray(list, j);
        if (!a[i].ours && neighbours != NULL)
        {
            neighbours[m].channel = a[i].channel;
            neighbours[m].rssi = a[i].rssi;
            m++;
        }
    }
    cp_unlock();
    cJSON_AddNumberToObject(out, "when", (double)when);
    if (neighbours != NULL)
    {
        cp_channels_t c;
        cp_channel_advice(neighbours, m, l.channel, &c);
        cJSON *adv = cJSON_AddObjectToObject(out, "channels");
        cJSON *score = cJSON_AddArrayToObject(adv, "score");
        for (int i = 1; i <= CP_CHANNELS; i++)
        {
            cJSON_AddItemToArray(score, cJSON_CreateNumber(c.score[i]));
        }
        cJSON_AddNumberToObject(adv, "best", c.best);
        cJSON_AddNumberToObject(adv, "current", c.current);
        cJSON_AddBoolToObject(adv, "change", c.worth_changing);
        free(neighbours);
    }
    return appkit_json(request, out);
}

static int floor_of(httpd_req_t *request)
{
    char value[8];
    appkit_query(request, "floor", value, sizeof(value));
    int floor = atoi(value);
    return floor < 0 || floor >= CP_MAX_FLOORS ? 0 : floor;
}

static esp_err_t plan_get(httpd_req_t *request)
{
    GUARD();
    char path[80];
    if (!cp_plan_path(floor_of(request), path, sizeof(path)) || !storage_files_acquire())
    {
        return appkit_error(request, "404 Not Found", "Nessuna piantina");
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        storage_files_release();
        return appkit_error(request, "404 Not Found", "Nessuna piantina");
    }
    char *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    size_t got = chunk != NULL ? fread(chunk, 1, 4096, file) : 0;
    bool png = got > 4 && memcmp(chunk, "\x89PNG", 4) == 0;
    httpd_resp_set_type(request, png ? "image/png" : "image/jpeg");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = ESP_OK;
    while (got > 0 && err == ESP_OK)
    {
        err = httpd_resp_send_chunk(request, chunk, (ssize_t)got);
        got = fread(chunk, 1, 4096, file);
    }
    fclose(file);
    storage_files_release();
    free(chunk);
    httpd_resp_send_chunk(request, NULL, 0);
    return err;
}

static esp_err_t plan_post(httpd_req_t *request)
{
    GUARD();
    if (request->content_len == 0 || request->content_len > PLAN_MAX)
    {
        return appkit_error(request, "413 Payload Too Large", "Immagine troppo grande (max 900 KB)");
    }
    char path[80], temporary[88];
    if (!cp_plan_path(floor_of(request), path, sizeof(path)) || !storage_files_acquire())
    {
        return appkit_error(request, "503 Service Unavailable", "Memoria non disponibile");
    }
    snprintf(temporary, sizeof(temporary), "%s.new", path);
    FILE *file = fopen(temporary, "wb");
    char *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    size_t left = request->content_len;
    bool ok = file != NULL && chunk != NULL;
    int timeouts = 0;
    while (ok && left > 0)
    {
        int got = httpd_req_recv(request, chunk, left < 4096 ? left : 4096);
        if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4)
        {
            continue;
        }
        ok = got > 0 && fwrite(chunk, 1, (size_t)got, file) == (size_t)got;
        left -= got > 0 ? (size_t)got : 0;
    }
    if (file != NULL)
    {
        ok = fclose(file) == 0 && ok;
    }
    if (ok)
    {
        unlink(path);
        ok = rename(temporary, path) == 0;
    }
    else
    {
        unlink(temporary);
    }
    storage_files_release();
    free(chunk);
    if (!ok)
    {
        return appkit_error(request, "500 Internal Server Error", "Salvataggio non riuscito");
    }
    cp_lock();
    cp_changed(false);
    cp_unlock();
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t plan_delete(httpd_req_t *request)
{
    GUARD();
    char path[80];
    if (cp_plan_path(floor_of(request), path, sizeof(path)) && storage_files_acquire())
    {
        unlink(path);
        storage_files_release();
    }
    cp_lock();
    cp_changed(false);
    cp_unlock();
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t measure(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    const cJSON *room = cJSON_GetObjectItemCaseSensitive(root, "room");
    bool ok = cJSON_IsString(room) && room->valuestring[0] != '\0' &&
              cp_measure(room->valuestring, (uint8_t)num(root, "floor", 0));
    cJSON_Delete(root);
    return ok ? appkit_json(request, cJSON_CreateObject())
              : appkit_error(request, "409 Conflict", "Misura già in corso o Wi-Fi non collegato");
}

static esp_err_t place(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    bool ok = cp_point_place((uint32_t)num(root, "id", 0), (int)num(root, "x", -1), (int)num(root, "y", -1),
                             (int)num(root, "floor", -1));
    cJSON_Delete(root);
    return ok ? appkit_json(request, cJSON_CreateObject()) : appkit_error(request, "404 Not Found", "Punto non trovato");
}

static esp_err_t remove_point(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    bool ok = cp_point_delete((uint32_t)num(root, "id", 0));
    cJSON_Delete(root);
    return ok ? appkit_json(request, cJSON_CreateObject()) : appkit_error(request, "404 Not Found", "Punto non trovato");
}

static esp_err_t clear(httpd_req_t *request)
{
    GUARD();
    cp_points_clear();
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t rooms_post(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    static char list[CP_MAX_ROOMS][CP_ROOM];
    int n = 0;
    const cJSON *item;
    const cJSON *rooms = cJSON_GetObjectItemCaseSensitive(root, "rooms");
    if (cJSON_IsArray(rooms))
    {
        cJSON_ArrayForEach(item, rooms)
        {
            if (cJSON_IsString(item) && n < CP_MAX_ROOMS)
            {
                snprintf(list[n++], CP_ROOM, "%.20s", item->valuestring);
            }
        }
        cp_rooms_set((const char (*)[CP_ROOM])list, n);
    }
    const cJSON *floors = cJSON_GetObjectItemCaseSensitive(root, "floors");
    if (cJSON_IsArray(floors))
    {
        n = 0;
        cJSON_ArrayForEach(item, floors)
        {
            if (cJSON_IsString(item) && n < CP_MAX_FLOORS)
            {
                snprintf(list[n++], CP_ROOM, "%.20s", item->valuestring);
            }
        }
        cp_floors_set((const char (*)[CP_ROOM])list, n);
    }
    cJSON_Delete(root);
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t scan_post(httpd_req_t *request)
{
    GUARD();
    cp_scan_now();
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t finder_post(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    int mode = (int)num(root, "mode", -1);
    cJSON_Delete(root);
    if (mode < 0)
    {
        cp_finder_stop();
    }
    else
    {
        cp_finder_start(mode ? 1 : 0);
    }
    return appkit_json(request, cJSON_CreateObject());
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/state", .method = HTTP_GET, .handler = state},
    {.uri = "/api/history", .method = HTTP_GET, .handler = history},
    {.uri = "/api/points", .method = HTTP_GET, .handler = points},
    {.uri = "/api/aps", .method = HTTP_GET, .handler = aps},
    {.uri = "/api/plan", .method = HTTP_GET, .handler = plan_get},
    {.uri = "/api/plan", .method = HTTP_POST, .handler = plan_post},
    {.uri = "/api/plan/delete", .method = HTTP_POST, .handler = plan_delete},
    {.uri = "/api/measure", .method = HTTP_POST, .handler = measure},
    {.uri = "/api/point/place", .method = HTTP_POST, .handler = place},
    {.uri = "/api/point/delete", .method = HTTP_POST, .handler = remove_point},
    {.uri = "/api/points/clear", .method = HTTP_POST, .handler = clear},
    {.uri = "/api/rooms", .method = HTTP_POST, .handler = rooms_post},
    {.uri = "/api/scan", .method = HTTP_POST, .handler = scan_post},
    {.uri = "/api/finder", .method = HTTP_POST, .handler = finder_post},
};

static const appkit_web_config_t s_config = {
    .name = "Copertura",
    .page_start = dashboard_html_start,
    .page_end = dashboard_html_end,
    .routes = s_routes,
    .route_count = sizeof(s_routes) / sizeof(s_routes[0]),
};

esp_err_t copertura_web_start(void)
{
    return appkit_web_start(&s_config);
}
