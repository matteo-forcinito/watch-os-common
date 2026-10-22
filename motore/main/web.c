/*
 * Motore dashboard API (served by appkit, every route needs the paired token).
 *
 *   GET  /api/state         phase, adapter, protocol, VIN, lock, settings, values, trip
 *   GET  /api/codes         trouble codes with descriptions, readiness, freeze frame
 *   POST /api/codes/read
 *   POST /api/codes/clear   {} -> {ok, message}  (unlocked, car stopped, engine off)
 *   POST /api/unlock        {pin}      POST /api/lock
 *   POST /api/settings      {link, host, port, fuel_type, displacement_cc, volumetric, pin, pin_old}
 *   GET  /api/scan          adapters found    POST /api/scan   starts a scan
 *   POST /api/choose        {addr:"0:aa:bb:cc:dd:ee:ff", name}
 *   POST /api/trip/reset
 *   POST /api/console       {cmd} -> {ok, out}  (read-only commands)
 *
 * The PIN never leaves the device: the state says only whether one is set.
 */
#include "motore.h"

#include "appkit.h"

#include "esp_timer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char dashboard_html_start[] asm("_binary_motore_dashboard_html_start");
extern const char dashboard_html_end[] asm("_binary_motore_dashboard_html_end");

static double number(const cJSON *root, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static const char *text(const cJSON *root, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, key));
}

static cJSON *body(httpd_req_t *request)
{
    char *raw = appkit_body(request, 1024);
    cJSON *root = raw != NULL ? cJSON_Parse(raw) : NULL;
    free(raw);
    return root != NULL ? root : cJSON_CreateObject();
}

static void add_settings(cJSON *out)
{
    mt_settings_t s;
    mt_settings(&s);
    cJSON *j = cJSON_AddObjectToObject(out, "settings");
    cJSON_AddNumberToObject(j, "link", s.link);
    cJSON_AddStringToObject(j, "ble_name", s.ble_name);
    cJSON_AddStringToObject(j, "host", s.host);
    cJSON_AddNumberToObject(j, "port", s.port);
    cJSON_AddBoolToObject(j, "pin_set", s.pin[0] != 0);
    cJSON_AddNumberToObject(j, "fuel_type", s.fuel_type);
    cJSON_AddNumberToObject(j, "displacement_cc", s.displacement_cc);
    cJSON_AddNumberToObject(j, "volumetric", s.volumetric);
    cJSON_AddNumberToObject(j, "shift_rpm", s.shift_rpm);
}

static esp_err_t state(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    cJSON *out = cJSON_CreateObject();
    mt_lock();
    const mt_state_t *s = mt_state();
    cJSON_AddNumberToObject(out, "phase", s->phase);
    cJSON_AddStringToObject(out, "message", s->message);
    cJSON_AddStringToObject(out, "adapter", s->adapter);
    cJSON_AddStringToObject(out, "protocol", s->protocol);
    cJSON_AddStringToObject(out, "vin", s->vin);
    cJSON_AddBoolToObject(out, "unlocked", s->unlocked);
    cJSON_AddBoolToObject(out, "mil", s->mil);
    cJSON_AddNumberToObject(out, "dtc_count", s->dtc_count);
    cJSON_AddNumberToObject(out, "dtcs", s->dtcs);
    cJSON_AddNumberToObject(out, "polls", s->polls_per_s);
    cJSON_AddNumberToObject(out, "revision", s->revision);
    cJSON *values = cJSON_AddArrayToObject(out, "values");
    for (int v = 0; v < MT_VALUES; v++)
    {
        const mt_value_info_t *info = mt_value_info((mt_value_t)v);
        bool fresh = mt_value_fresh(s, (mt_value_t)v);
        bool has = s->updated[v] != 0;
        if (!has && v != MT_BATTERY && !(info->pid && (s->supported[(info->pid - 1) / 8] & (0x80 >> ((info->pid - 1) % 8)))))
        {
            continue;   /* the car does not have it */
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "key", info->key);
        cJSON_AddStringToObject(j, "name", info->name);
        cJSON_AddStringToObject(j, "unit", info->unit);
        cJSON_AddNumberToObject(j, "min", info->min);
        cJSON_AddNumberToObject(j, "max", info->max);
        cJSON_AddNumberToObject(j, "decimals", info->decimals);
        if (has)
        {
            cJSON_AddNumberToObject(j, "v", s->value[v]);
        }
        cJSON_AddBoolToObject(j, "fresh", fresh);
        cJSON_AddItemToArray(values, j);
    }
    cJSON *trip = cJSON_AddObjectToObject(out, "trip");
    cJSON_AddNumberToObject(trip, "km", s->trip.km);
    cJSON_AddNumberToObject(trip, "litres", s->trip.litres);
    cJSON_AddNumberToObject(trip, "seconds", s->trip.seconds);
    cJSON_AddNumberToObject(trip, "idle_seconds", s->trip.idle_seconds);
    cJSON_AddNumberToObject(trip, "max_speed", s->trip.max_speed);
    cJSON_AddNumberToObject(trip, "max_rpm", s->trip.max_rpm);
    mt_unlock();
    add_settings(out);
    char hotspot[34];
    appkit_web_hotspot(hotspot, sizeof(hotspot));
    cJSON_AddStringToObject(out, "hotspot", hotspot);
    return appkit_json(request, out);
}

static const char *kind_name(uint8_t kind)
{
    return kind == MT_DTC_PENDING ? "in attesa" : kind == MT_DTC_PERMANENT ? "permanente" : "memorizzato";
}

static esp_err_t codes(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    cJSON *out = cJSON_CreateObject();
    mt_lock();
    const mt_state_t *s = mt_state();
    cJSON_AddBoolToObject(out, "mil", s->mil);
    cJSON_AddNumberToObject(out, "read_ago", s->dtc_read ? (double)(esp_timer_get_time() - s->dtc_read) / 1e6 : -1);
    cJSON *list = cJSON_AddArrayToObject(out, "codes");
    for (int i = 0; i < s->dtcs; i++)
    {
        char buf[96];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "code", s->dtc[i].code);
        cJSON_AddStringToObject(j, "kind", kind_name(s->dtc[i].kind));
        cJSON_AddStringToObject(j, "system", mt_dtc_system(s->dtc[i].code));
        cJSON_AddStringToObject(j, "text", mt_dtc_text(s->dtc[i].code, buf, sizeof(buf)));
        cJSON_AddItemToArray(list, j);
    }
    cJSON *monitors = cJSON_AddArrayToObject(out, "monitors");
    for (int i = 0; i < MT_MONITORS; i++)
    {
        if (s->monitor[i].name == NULL || !s->monitor[i].supported)
        {
            continue;
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", s->monitor[i].name);
        cJSON_AddBoolToObject(j, "complete", s->monitor[i].complete);
        cJSON_AddItemToArray(monitors, j);
    }
    cJSON_AddStringToObject(out, "freeze_code", s->freeze_code);
    cJSON *freeze = cJSON_AddArrayToObject(out, "freeze");
    for (int v = 0; v < MT_VALUES; v++)
    {
        if (isnan(s->freeze[v]))
        {
            continue;
        }
        const mt_value_info_t *info = mt_value_info((mt_value_t)v);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", info->name);
        cJSON_AddStringToObject(j, "unit", info->unit);
        cJSON_AddNumberToObject(j, "v", s->freeze[v]);
        cJSON_AddNumberToObject(j, "decimals", info->decimals);
        cJSON_AddItemToArray(freeze, j);
    }
    mt_unlock();
    return appkit_json(request, out);
}

static esp_err_t codes_read(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    mt_read_codes();
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t codes_clear(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    char why[96];
    esp_err_t err = mt_clear_codes(why, sizeof(why));
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
    cJSON_AddStringToObject(out, "message", why);
    return appkit_json(request, out);
}

static esp_err_t unlock(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    cJSON *root = body(request);
    bool ok = mt_unlock_actions(text(root, "pin"));
    cJSON_Delete(root);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    return appkit_json(request, out);
}

static esp_err_t lock(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    mt_lock_actions();
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t settings(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    cJSON *root = body(request);
    mt_settings_t s;
    mt_settings(&s);
    s.link = (uint8_t)number(root, "link", s.link) ? MT_LINK_WIFI : MT_LINK_BLE;
    const char *host = text(root, "host");
    if (host != NULL)
    {
        snprintf(s.host, sizeof(s.host), "%s", host);
    }
    s.port = (uint16_t)number(root, "port", s.port);
    s.fuel_type = (uint8_t)number(root, "fuel_type", s.fuel_type);
    s.displacement_cc = (uint16_t)number(root, "displacement_cc", s.displacement_cc);
    s.volumetric = (float)number(root, "volumetric", s.volumetric);
    s.shift_rpm = (uint16_t)number(root, "shift_rpm", s.shift_rpm);
    const char *pin = text(root, "pin");
    const char *old = text(root, "pin_old");
    bool pin_refused = false;
    if (pin != NULL && pin[0] != 0)
    {
        size_t len = strlen(pin);
        bool digits = len >= 4 && len <= 8 && strspn(pin, "0123456789") == len;
        if (!digits || (s.pin[0] != 0 && (old == NULL || strcmp(old, s.pin) != 0)))
        {
            pin_refused = true;   /* changing it needs the current one */
        }
        else
        {
            snprintf(s.pin, sizeof(s.pin), "%s", pin);
        }
    }
    cJSON_Delete(root);
    if (pin_refused)
    {
        return appkit_error(request, "403 Forbidden", "Codice attuale errato o nuovo codice non valido (4-8 cifre)");
    }
    mt_set_settings(&s);
    cJSON *out = cJSON_CreateObject();
    add_settings(out);
    return appkit_json(request, out);
}

static esp_err_t scan_get(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    mt_found_t found[MT_SCAN_MAX];
    int n = mt_scan_results(found, MT_SCAN_MAX);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "scanning", mt_link_scanning());
    cJSON *list = cJSON_AddArrayToObject(out, "found");
    for (int i = 0; i < n; i++)
    {
        char addr[24];
        snprintf(addr, sizeof(addr), "%u:%02x:%02x:%02x:%02x:%02x:%02x", found[i].addr[0], found[i].addr[6], found[i].addr[5],
                 found[i].addr[4], found[i].addr[3], found[i].addr[2], found[i].addr[1]);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "addr", addr);
        cJSON_AddStringToObject(j, "name", found[i].name[0] ? found[i].name : "(senza nome)");
        cJSON_AddNumberToObject(j, "rssi", found[i].rssi);
        cJSON_AddBoolToObject(j, "likely", found[i].likely);
        cJSON_AddItemToArray(list, j);
    }
    return appkit_json(request, out);
}

static esp_err_t scan_post(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    mt_scan();
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t choose(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    cJSON *root = body(request);
    const char *addr = text(root, "addr");
    const char *name = text(root, "name");
    unsigned t, b[6];
    bool ok = addr != NULL && sscanf(addr, "%u:%x:%x:%x:%x:%x:%x", &t, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 7;
    if (ok)
    {
        mt_found_t f = {0};
        f.addr[0] = (uint8_t)t;
        for (int i = 0; i < 6; i++)
        {
            f.addr[6 - i] = (uint8_t)b[i];   /* shown most significant first, stored little-endian */
        }
        snprintf(f.name, sizeof(f.name), "%s", name != NULL ? name : "");
        mt_choose_ble(&f);
    }
    cJSON_Delete(root);
    return ok ? appkit_json(request, cJSON_CreateObject()) : appkit_error(request, "400 Bad Request", "Indirizzo non valido");
}

static esp_err_t trip_reset(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    mt_trip_reset();
    return appkit_json(request, cJSON_CreateObject());
}

static esp_err_t console(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    cJSON *root = body(request);
    const char *command = text(root, "cmd");
    char out[512] = "";
    esp_err_t err = command != NULL ? mt_console(command, out, sizeof(out)) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", err == ESP_OK);
    cJSON_AddStringToObject(reply, "out", out);
    return appkit_json(request, reply);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/state", .method = HTTP_GET, .handler = state},
    {.uri = "/api/codes", .method = HTTP_GET, .handler = codes},
    {.uri = "/api/codes/read", .method = HTTP_POST, .handler = codes_read},
    {.uri = "/api/codes/clear", .method = HTTP_POST, .handler = codes_clear},
    {.uri = "/api/unlock", .method = HTTP_POST, .handler = unlock},
    {.uri = "/api/lock", .method = HTTP_POST, .handler = lock},
    {.uri = "/api/settings", .method = HTTP_POST, .handler = settings},
    {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get},
    {.uri = "/api/scan", .method = HTTP_POST, .handler = scan_post},
    {.uri = "/api/choose", .method = HTTP_POST, .handler = choose},
    {.uri = "/api/trip/reset", .method = HTTP_POST, .handler = trip_reset},
    {.uri = "/api/console", .method = HTTP_POST, .handler = console},
};

static const appkit_web_config_t s_config = {
    .name = "Motore",
    .page_start = dashboard_html_start,
    .page_end = dashboard_html_end,
    .routes = s_routes,
    .route_count = sizeof(s_routes) / sizeof(s_routes[0]),
    .hotspot = true,   /* in the car there is no known network */
};

esp_err_t motore_web_start(void)
{
    return appkit_web_start(&s_config);
}
