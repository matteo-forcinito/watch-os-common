/*
 * Pieno dashboard API (served by appkit, every route needs the paired token).
 *
 *   GET  /api/state          status, settings, area figures for every fuel/mode
 *   GET  /api/stations       every station in the radius (streamed in chunks)
 *   GET  /api/history        ?fuel=0&self=1 -> [[day,min,median,max,count],...]
 *   GET  /api/changes        ?id=123 -> "day,fuel,self,price" lines
 *   GET  /api/export.csv     today's prices as a spreadsheet
 *   POST /api/settings       {lat,lon,place,radius_km,fuel,self,real_cost,litres,km_per_litre,target}
 *   POST /api/refresh
 */
#include "pieno.h"

#include "appkit.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const char dashboard_html_end[] asm("_binary_dashboard_html_end");

static void add_settings(cJSON *out, const pn_settings_t *s)
{
    cJSON *j = cJSON_AddObjectToObject(out, "settings");
    cJSON_AddNumberToObject(j, "lat", s->lat);
    cJSON_AddNumberToObject(j, "lon", s->lon);
    cJSON_AddStringToObject(j, "place", s->place);
    cJSON_AddNumberToObject(j, "radius_km", s->radius_km);
    cJSON_AddNumberToObject(j, "fuel", s->fuel);
    cJSON_AddNumberToObject(j, "self", s->self);
    cJSON_AddBoolToObject(j, "real_cost", s->real_cost);
    cJSON_AddNumberToObject(j, "litres", s->litres);
    cJSON_AddNumberToObject(j, "km_per_litre", s->km_per_litre);
    cJSON_AddNumberToObject(j, "target", s->target);
    cJSON_AddNumberToObject(j, "max_age_days", s->max_age_days);
}

static esp_err_t state(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    pn_status_t st;
    pn_status(&st);
    pn_settings_t s;
    pn_settings(&s);
    cJSON *out = cJSON_CreateObject();
    cJSON *status = cJSON_AddObjectToObject(out, "status");
    cJSON_AddNumberToObject(status, "phase", st.phase);
    cJSON_AddNumberToObject(status, "percent", st.percent);
    cJSON_AddStringToObject(status, "message", st.message);
    cJSON_AddStringToObject(status, "extraction", st.extraction);
    cJSON_AddNumberToObject(status, "fetched", (double)st.fetched);
    cJSON_AddNumberToObject(status, "count", st.count);
    cJSON_AddNumberToObject(status, "revision", st.revision);
    cJSON_AddNumberToObject(status, "now", (double)pn_local_now());
    add_settings(out, &s);

    /* area[fuel][self] = {min, median, max, count, best} */
    cJSON *area = cJSON_AddArrayToObject(out, "area");
    pn_lock();
    int count = 0;
    const pn_station_t *table = pn_stations(&count);
    for (int f = 0; f < PN_FUELS; f++)
    {
        cJSON *modes = cJSON_CreateArray();
        for (int m = 0; m < 2; m++)
        {
            cJSON *a = cJSON_CreateObject();
            float lo, mid, hi;
            int n = 0;
            if (pn_area(f, m, &s, &lo, &mid, &hi, &n))
            {
                cJSON_AddNumberToObject(a, "min", lo);
                cJSON_AddNumberToObject(a, "median", mid);
                cJSON_AddNumberToObject(a, "max", hi);
                uint16_t order[1];
                if (pn_rank(f, m, &s, order, 1) == 1)
                {
                    cJSON_AddNumberToObject(a, "best", table[order[0]].id);
                }
            }
            cJSON_AddNumberToObject(a, "count", n);
            cJSON_AddItemToArray(modes, a);
        }
        cJSON_AddItemToArray(area, modes);
    }
    pn_unlock();
    return appkit_json(request, out);
}

static esp_err_t send_text(httpd_req_t *request, const char *text)
{
    return httpd_resp_send_chunk(request, text, HTTPD_RESP_USE_STRLEN);
}

/* JSON string escaping for our own short texts. */
static void quoted(char *out, size_t size, const char *in)
{
    size_t n = 0;
    if (size < 3)
    {
        return;
    }
    out[n++] = '"';
    for (; *in != '\0' && n < size - 3; in++)
    {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\')
        {
            out[n++] = '\\';
            out[n++] = (char)c;
        }
        else if (c >= 0x20)
        {
            out[n++] = (char)c;
        }
    }
    out[n++] = '"';
    out[n] = '\0';
}

static esp_err_t stations(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char *line = heap_caps_malloc(1600, MALLOC_CAP_SPIRAM);
    pn_station_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
    if (line == NULL || s == NULL)
    {
        free(line);
        free(s);
        return appkit_error(request, "503 Service Unavailable", "Memoria occupata");
    }
    esp_err_t err = send_text(request, "[");
    for (int i = 0; err == ESP_OK; i++)
    {
        int count = 0;
        pn_lock();
        const pn_station_t *table = pn_stations(&count);
        bool more = i < count;
        if (more)
        {
            *s = table[i];
        }
        pn_unlock();
        if (!more)
        {
            break;
        }
        char brand[64], name[110], address[140], town[72];
        quoted(brand, sizeof(brand), s->brand);
        quoted(name, sizeof(name), s->name);
        quoted(address, sizeof(address), s->address);
        quoted(town, sizeof(town), s->town);
        int n = snprintf(line, 1600, "%s{\"id\":%lu,\"brand\":%s,\"name\":%s,\"address\":%s,\"town\":%s,\"prov\":\"%.3s\","
                         "\"lat\":%.6f,\"lon\":%.6f,\"km\":%.2f,\"p\":[",
                         i ? "," : "", (unsigned long)s->id, brand, name, address, town, s->province,
                         (double)s->lat, (double)s->lon, (double)s->km);
        /* p[fuel] = [served, self, served_when, self_when, served_previous, self_previous] */
        for (int f = 0; f < PN_FUELS; f++)
        {
            n += snprintf(line + n, 1600 - n, "%s[%.3f,%.3f,%lld,%lld,%.3f,%.3f]", f ? "," : "",
                          (double)s->price[f][0], (double)s->price[f][1], (long long)s->when[f][0],
                          (long long)s->when[f][1], (double)s->previous[f][0], (double)s->previous[f][1]);
        }
        n += snprintf(line + n, 1600 - n, "],\"x\":[");
        for (int e = 0; e < s->extra_count && n < 1500; e++)
        {
            char product[56];
            quoted(product, sizeof(product), s->extra[e].product);
            n += snprintf(line + n, 1600 - n, "%s[%s,%.3f,%d]", e ? "," : "", product, (double)s->extra[e].price,
                          s->extra[e].self);
        }
        snprintf(line + n, 1600 - n, "]}");
        err = send_text(request, line);
    }
    if (err == ESP_OK)
    {
        err = send_text(request, "]");
    }
    free(line);
    free(s);
    httpd_resp_send_chunk(request, NULL, 0);
    return err;
}

static esp_err_t history(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    char value[8];
    appkit_query(request, "fuel", value, sizeof(value));
    int fuel = atoi(value) % PN_FUELS;
    appkit_query(request, "self", value, sizeof(value));
    int self = atoi(value) ? 1 : 0;
    pn_day_t *days = heap_caps_malloc(sizeof(pn_day_t) * PN_HISTORY_DAYS, MALLOC_CAP_SPIRAM);
    if (days == NULL)
    {
        return appkit_json(request, NULL);
    }
    int n = pn_history(fuel, self, days, PN_HISTORY_DAYS);
    cJSON *out = cJSON_CreateArray();
    for (int i = 0; i < n; i++)
    {
        cJSON *d = cJSON_CreateArray();
        cJSON_AddItemToArray(d, cJSON_CreateNumber(days[i].day));
        cJSON_AddItemToArray(d, cJSON_CreateNumber((int)(days[i].min * 1000 + 0.5f) / 1000.0));
        cJSON_AddItemToArray(d, cJSON_CreateNumber((int)(days[i].median * 1000 + 0.5f) / 1000.0));
        cJSON_AddItemToArray(d, cJSON_CreateNumber((int)(days[i].max * 1000 + 0.5f) / 1000.0));
        cJSON_AddItemToArray(d, cJSON_CreateNumber(days[i].count));
        cJSON_AddItemToArray(out, d);
    }
    free(days);
    return appkit_json(request, out);
}

static esp_err_t changes(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    char value[16];
    appkit_query(request, "id", value, sizeof(value));
    char *text = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (text == NULL)
    {
        return appkit_error(request, "503 Service Unavailable", "Memoria occupata");
    }
    pn_changes((uint32_t)strtoul(value, NULL, 10), text, 16384);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, text);
    free(text);
    return err;
}

static esp_err_t export_csv(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    httpd_resp_set_type(request, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(request, "Content-Disposition", "attachment; filename=\"pieno.csv\"");
    esp_err_t err = send_text(request, "id;marchio;nome;indirizzo;comune;km;carburante;modalita;prezzo\n");
    pn_station_t *s = heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
    char line[320];
    for (int i = 0; s != NULL && err == ESP_OK; i++)
    {
        int count = 0;
        pn_lock();
        const pn_station_t *table = pn_stations(&count);
        bool more = i < count;
        if (more)
        {
            *s = table[i];
        }
        pn_unlock();
        if (!more)
        {
            break;
        }
        for (int f = 0; f < PN_FUELS && err == ESP_OK; f++)
        {
            for (int m = 0; m < 2 && err == ESP_OK; m++)
            {
                if (s->price[f][m] > 0)
                {
                    snprintf(line, sizeof(line), "%lu;%s;%s;%s;%s;%.1f;%s;%s;%.3f\n", (unsigned long)s->id, s->brand,
                             s->name, s->address, s->town, (double)s->km, pn_fuel_names[f], m ? "self" : "servito",
                             (double)s->price[f][m]);
                    for (char *c = line; *c; c++)
                    {
                        if (*c == '.' && c > line && c[-1] >= '0' && c[-1] <= '9' && c[1] >= '0' && c[1] <= '9')
                        {
                            *c = ',';   /* Italian spreadsheets */
                        }
                    }
                    err = send_text(request, line);
                }
            }
        }
    }
    free(s);
    httpd_resp_send_chunk(request, NULL, 0);
    return err;
}

static double number(const cJSON *root, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valuedouble : cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static esp_err_t settings(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    char *text = appkit_body(request, 1024);
    cJSON *root = text != NULL ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return appkit_error(request, "400 Bad Request", "Dati non validi");
    }
    pn_settings_t s;
    pn_settings(&s);
    double lat = number(root, "lat", s.lat), lon = number(root, "lon", s.lon);
    if (lat < 35 || lat > 48 || lon < 6 || lon > 19)
    {
        cJSON_Delete(root);
        return appkit_error(request, "400 Bad Request", "La posizione deve essere in Italia");
    }
    s.lat = (float)lat;
    s.lon = (float)lon;
    const cJSON *place = cJSON_GetObjectItemCaseSensitive(root, "place");
    if (cJSON_IsString(place))
    {
        snprintf(s.place, sizeof(s.place), "%s", place->valuestring);
    }
    s.radius_km = (uint8_t)number(root, "radius_km", s.radius_km);
    s.fuel = (uint8_t)number(root, "fuel", s.fuel);
    s.self = (uint8_t)number(root, "self", s.self);
    s.real_cost = number(root, "real_cost", s.real_cost) != 0;
    s.litres = (float)number(root, "litres", s.litres);
    s.km_per_litre = (float)number(root, "km_per_litre", s.km_per_litre);
    s.target = (float)number(root, "target", s.target);
    s.max_age_days = (uint8_t)number(root, "max_age_days", s.max_age_days);
    cJSON_Delete(root);
    pn_set_settings(&s);
    pn_settings(&s);
    cJSON *out = cJSON_CreateObject();
    add_settings(out, &s);
    return appkit_json(request, out);
}

static esp_err_t refresh(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    pn_refresh();
    return appkit_json(request, cJSON_CreateObject());
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/state", .method = HTTP_GET, .handler = state},
    {.uri = "/api/stations", .method = HTTP_GET, .handler = stations},
    {.uri = "/api/history", .method = HTTP_GET, .handler = history},
    {.uri = "/api/changes", .method = HTTP_GET, .handler = changes},
    {.uri = "/api/export.csv", .method = HTTP_GET, .handler = export_csv},
    {.uri = "/api/settings", .method = HTTP_POST, .handler = settings},
    {.uri = "/api/refresh", .method = HTTP_POST, .handler = refresh},
};

static const appkit_web_config_t s_config = {
    .name = "Pieno",
    .page_start = dashboard_html_start,
    .page_end = dashboard_html_end,
    .routes = s_routes,
    .route_count = sizeof(s_routes) / sizeof(s_routes[0]),
};

esp_err_t pieno_web_start(void)
{
    return appkit_web_start(&s_config);
}
