/*
 * Cielo data feed (see cielo.h). One worker task owns Wi-Fi and HTTP:
 *
 *   every 12 s  aircraft within the radius  (api.adsb.lol/v2/point/lat/lon/nm),
 *               backing off up to 2 minutes when the service refuses (HTTP 429)
 *   every 20 s  International Space Station (api.wheretheiss.at)
 *   on demand   route of one callsign       (api.adsbdb.com/v0/callsign/X), cached
 *
 * JSON is parsed with cJSON allocating in PSRAM: a busy sky is ~100 KB of text
 * and thousands of small nodes, too much for the internal RAM.
 */
#include "cielo.h"

#include "cJSON.h"
#include "core/settings.h"
#include "core/sys.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/http_stream.h"
#include "services/wifi.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cielo";

#define JSON_SIZE (160 * 1024)
#define ROUTES 24
#define AIRCRAFT_PERIOD_MS 12000
#define BACKOFF_MAX_MS 120000
#define ISS_PERIOD_MS 20000
#define KM_PER_DEG 111.32f

static cielo_aircraft_t *s_table; /* PSRAM */
static int s_count;
static cielo_route_t s_routes[ROUTES];
static int s_route_next;
static char s_route_wanted[10];
static SemaphoreHandle_t s_lock;
static cielo_status_t s_status;
static char *s_json;
static TaskHandle_t s_task;
static atomic_int s_radius = 60;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void *psram_malloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static void set_message(const char *text)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.message, sizeof(s_status.message), "%s", text);
    s_status.revision++;
    xSemaphoreGive(s_lock);
}

static void copy_string(char *out, size_t size, const cJSON *item)
{
    out[0] = 0;
    if (!cJSON_IsString(item))
        return;
    snprintf(out, size, "%s", item->valuestring);
    /* Callsigns are padded with spaces. */
    for (int i = (int)strlen(out) - 1; i >= 0 && out[i] == ' '; i--)
        out[i] = 0;
}

static double number(const cJSON *item, double fallback) { return cJSON_IsNumber(item) ? item->valuedouble : fallback; }

/* ------------------------------------------------------------ aircraft */

static void parse_aircraft(const cJSON *root, float lat0, float lon0)
{
    const cJSON *list = cJSON_GetObjectItem(root, "ac");
    if (!cJSON_IsArray(list))
        return;
    float lon_scale = KM_PER_DEG * cosf(lat0 * (float)M_PI / 180.0f);
    uint32_t now = now_ms();
    cielo_aircraft_t *fresh = heap_caps_calloc(CIELO_AIRCRAFT, sizeof(cielo_aircraft_t), MALLOC_CAP_SPIRAM);
    if (fresh == NULL)
        return;
    int count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, list)
    {
        if (count == CIELO_AIRCRAFT)
            break;
        const cJSON *lat = cJSON_GetObjectItem(item, "lat"), *lon = cJSON_GetObjectItem(item, "lon");
        if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon))
            continue;
        cielo_aircraft_t *a = &fresh[count];
        copy_string(a->hex, sizeof(a->hex), cJSON_GetObjectItem(item, "hex"));
        copy_string(a->callsign, sizeof(a->callsign), cJSON_GetObjectItem(item, "flight"));
        copy_string(a->registration, sizeof(a->registration), cJSON_GetObjectItem(item, "r"));
        copy_string(a->type, sizeof(a->type), cJSON_GetObjectItem(item, "t"));
        copy_string(a->squawk, sizeof(a->squawk), cJSON_GetObjectItem(item, "squawk"));
        const cJSON *altitude = cJSON_GetObjectItem(item, "alt_baro");
        a->altitude_m = cJSON_IsNumber(altitude) ? (int32_t)(altitude->valuedouble * 0.3048) : INT32_MIN;
        a->speed_kmh = (int32_t)(number(cJSON_GetObjectItem(item, "gs"), 0) * 1.852);
        a->vertical_mpm = (int32_t)(number(cJSON_GetObjectItem(item, "baro_rate"), 0) * 0.3048);
        a->track_deg = (float)number(cJSON_GetObjectItem(item, "track"), 0);
        a->north_km = (float)(lat->valuedouble - lat0) * KM_PER_DEG;
        a->east_km = (float)(lon->valuedouble - lon0) * lon_scale;
        /* Position age: the report can be a few seconds old. */
        uint32_t age = (uint32_t)(number(cJSON_GetObjectItem(item, "seen_pos"), 0) * 1000);
        a->report_ms = now - (age < 60000 ? age : 0);
        const cJSON *emergency = cJSON_GetObjectItem(item, "emergency");
        a->emergency = (cJSON_IsString(emergency) && strcmp(emergency->valuestring, "none") != 0) ||
                       strcmp(a->squawk, "7700") == 0 || strcmp(a->squawk, "7600") == 0 ||
                       strcmp(a->squawk, "7500") == 0;
        if (!a->hex[0])
            continue;
        count++;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Keep each aircraft's trail: the previous report becomes a trail point. */
    for (int i = 0; i < count; i++)
        for (int k = 0; k < s_count; k++)
            if (strcmp(s_table[k].hex, fresh[i].hex) == 0)
            {
                const cielo_aircraft_t *old = &s_table[k];
                memcpy(fresh[i].trail_e, old->trail_e, sizeof(old->trail_e));
                memcpy(fresh[i].trail_n, old->trail_n, sizeof(old->trail_n));
                fresh[i].trail_count = old->trail_count;
                float de = fresh[i].east_km - old->east_km, dn = fresh[i].north_km - old->north_km;
                if (de * de + dn * dn > 0.04f) /* moved more than 200 m */
                {
                    if (fresh[i].trail_count == CIELO_TRAIL)
                    {
                        memmove(fresh[i].trail_e, fresh[i].trail_e + 1, sizeof(float) * (CIELO_TRAIL - 1));
                        memmove(fresh[i].trail_n, fresh[i].trail_n + 1, sizeof(float) * (CIELO_TRAIL - 1));
                        fresh[i].trail_count--;
                    }
                    fresh[i].trail_e[fresh[i].trail_count] = old->east_km;
                    fresh[i].trail_n[fresh[i].trail_count] = old->north_km;
                    fresh[i].trail_count++;
                }
                break;
            }
    memcpy(s_table, fresh, sizeof(cielo_aircraft_t) * CIELO_AIRCRAFT);
    s_count = count;
    s_status.count = count;
    s_status.updated_ms = now;
    s_status.online = true;
    snprintf(s_status.message, sizeof(s_status.message), "%d %s nel raggio di %d km", count,
             count == 1 ? "velivolo" : "velivoli", atomic_load(&s_radius));
    s_status.revision++;
    xSemaphoreGive(s_lock);
    free(fresh);
}

static bool fetch_aircraft(float lat, float lon)
{
    char url[128];
    int nm = (int)(atomic_load(&s_radius) / 1.852f + 0.5f);
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", lat, lon, nm);
    esp_err_t error = http_stream_get_text(url, s_json, JSON_SIZE, NULL, 0);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "Aircraft: %s", esp_err_to_name(error));
        return false;
    }
    cJSON *root = cJSON_Parse(s_json);
    if (root == NULL)
        return false;
    parse_aircraft(root, lat, lon);
    cJSON_Delete(root);
    return true;
}

/* ------------------------------------------------------------ space station */

static void fetch_iss(float lat0, float lon0)
{
    if (http_stream_get_text("https://api.wheretheiss.at/v1/satellites/25544", s_json, 4096, NULL, 0) != ESP_OK)
        return;
    cJSON *root = cJSON_Parse(s_json);
    if (root == NULL)
        return;
    double lat = number(cJSON_GetObjectItem(root, "latitude"), 999), lon = number(cJSON_GetObjectItem(root, "longitude"), 999);
    double altitude = number(cJSON_GetObjectItem(root, "altitude"), 0);
    const cJSON *visibility = cJSON_GetObjectItem(root, "visibility");
    if (lat < 900)
    {
        /* Great-circle distance and initial bearing. */
        double p1 = lat0 * M_PI / 180, p2 = lat * M_PI / 180, dl = (lon - lon0) * M_PI / 180;
        double a = sin((p2 - p1) / 2) * sin((p2 - p1) / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
        double km = 6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
        double bearing = atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) * 180 / M_PI;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.iss_valid = true;
        s_status.iss_km = (float)km;
        s_status.iss_bearing = (float)fmod(bearing + 360, 360);
        s_status.iss_altitude_km = (float)altitude;
        snprintf(s_status.iss_over, sizeof(s_status.iss_over), "%s",
                 cJSON_IsString(visibility) && strcmp(visibility->valuestring, "eclipsed") == 0 ? "nell'ombra della Terra"
                                                                                             : "illuminata dal Sole");
        s_status.revision++;
        xSemaphoreGive(s_lock);
    }
    cJSON_Delete(root);
}

/* ------------------------------------------------------------ routes */

static void airport(const cJSON *node, char *code, size_t code_size, char *city, size_t city_size)
{
    copy_string(code, code_size, cJSON_GetObjectItem(node, "iata_code"));
    if (!code[0])
        copy_string(code, code_size, cJSON_GetObjectItem(node, "icao_code"));
    copy_string(city, city_size, cJSON_GetObjectItem(node, "municipality"));
}

static void fetch_route(const char *callsign)
{
    char url[96];
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", callsign);
    cielo_route_t route = {.known = false};
    snprintf(route.callsign, sizeof(route.callsign), "%s", callsign);
    if (http_stream_get_text(url, s_json, 8192, NULL, 0) == ESP_OK)
    {
        cJSON *root = cJSON_Parse(s_json);
        const cJSON *flight = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "response"), "flightroute");
        if (cJSON_IsObject(flight))
        {
            copy_string(route.airline, sizeof(route.airline),
                        cJSON_GetObjectItem(cJSON_GetObjectItem(flight, "airline"), "name"));
            airport(cJSON_GetObjectItem(flight, "origin"), route.from_code, sizeof(route.from_code), route.from_city,
                    sizeof(route.from_city));
            airport(cJSON_GetObjectItem(flight, "destination"), route.to_code, sizeof(route.to_code), route.to_city,
                    sizeof(route.to_city));
            route.known = route.from_code[0] && route.to_code[0];
        }
        cJSON_Delete(root);
    }
    /* Unknown routes are cached too: no hammering for private flights. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_routes[s_route_next] = route;
    s_route_next = (s_route_next + 1) % ROUTES;
    if (strcmp(s_route_wanted, callsign) == 0)
        s_route_wanted[0] = 0;
    s_status.revision++;
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------ worker */

static void feed_task(void *arg)
{
    (void)arg;
    bool online = false;
    uint32_t next_aircraft = 0, next_iss = 0, backoff = AIRCRAFT_PERIOD_MS;
    for (;;)
    {
        const settings_t *settings = settings_get();
        float lat = settings->weather_lat, lon = settings->weather_lon;
        bool located = !(lat == 0 && lon == 0);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.located = located;
        s_status.lat = lat;
        s_status.lon = lon;
        s_status.radius_km = atomic_load(&s_radius);
        xSemaphoreGive(s_lock);
        if (!located)
        {
            set_message("Imposta la tua citta nel Meteo");
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }
        if (!online)
        {
            set_message("Connessione al Wi-Fi...");
            online = wifi_acquire(20000) == ESP_OK;
            if (!online)
            {
                set_message("Wi-Fi non disponibile, riprovo");
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
                continue;
            }
        }
        uint32_t now = now_ms();
        if ((int32_t)(now - next_aircraft) >= 0)
        {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.updating = true;
            xSemaphoreGive(s_lock);
            if (fetch_aircraft(lat, lon))
                backoff = AIRCRAFT_PERIOD_MS;
            else
            {
                /* Busy or unreachable: wait longer each time, positions keep gliding meanwhile. */
                backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
                char text[64];
                snprintf(text, sizeof(text), "Rete ADS-B occupata, riprovo tra %lu s", (unsigned long)(backoff / 1000));
                set_message(text);
            }
            next_aircraft = now_ms() + backoff;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.updating = false;
            xSemaphoreGive(s_lock);
        }
        char wanted[10];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(wanted, sizeof(wanted), "%s", s_route_wanted);
        xSemaphoreGive(s_lock);
        if (wanted[0])
            fetch_route(wanted);
        else if ((int32_t)(now_ms() - next_iss) >= 0)
        {
            fetch_iss(lat, lon);
            next_iss = now_ms() + ISS_PERIOD_MS;
        }
        int32_t wait = (int32_t)(next_aircraft - now_ms());
        if (wait > 0)
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait < 1000 ? wait : 1000));
    }
}

esp_err_t cielo_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_table = heap_caps_calloc(CIELO_AIRCRAFT, sizeof(cielo_aircraft_t), MALLOC_CAP_SPIRAM);
    s_json = heap_caps_malloc(JSON_SIZE, MALLOC_CAP_SPIRAM);
    if (s_lock == NULL || s_table == NULL || s_json == NULL)
        return ESP_ERR_NO_MEM;
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    snprintf(s_status.message, sizeof(s_status.message), "Avvio...");
    if (!sys_task_create(feed_task, "cielo_feed", 8192, NULL, 4, tskNO_AFFINITY))
        return ESP_ERR_NO_MEM;
    s_task = xTaskGetHandle("cielo_feed");
    return ESP_OK;
}

/* ------------------------------------------------------------ queries */

void cielo_status(cielo_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

int cielo_aircraft(cielo_aircraft_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int count = s_count < max ? s_count : max;
    memcpy(out, s_table, sizeof(cielo_aircraft_t) * (size_t)count);
    xSemaphoreGive(s_lock);
    return count;
}

bool cielo_find(const char *hex, cielo_aircraft_t *out)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && !found; i++)
        if (strcmp(s_table[i].hex, hex) == 0)
        {
            *out = s_table[i];
            found = true;
        }
    xSemaphoreGive(s_lock);
    return found;
}

bool cielo_route(const char *callsign, cielo_route_t *out)
{
    if (callsign == NULL || !callsign[0])
        return false;
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < ROUTES && !found; i++)
        if (strcmp(s_routes[i].callsign, callsign) == 0)
        {
            *out = s_routes[i];
            found = true;
        }
    if (!found && !s_route_wanted[0])
        snprintf(s_route_wanted, sizeof(s_route_wanted), "%s", callsign);
    xSemaphoreGive(s_lock);
    if (!found && s_task)
        xTaskNotifyGive(s_task);
    return found && out->known;
}

void cielo_set_radius(int km)
{
    atomic_store(&s_radius, km < 10 ? 10 : km > 200 ? 200 : km);
    cielo_refresh_now();
}

void cielo_refresh_now(void)
{
    if (s_task)
        xTaskNotifyGive(s_task);
}

void cielo_position_now(const cielo_aircraft_t *a, uint32_t now, float *east_km, float *north_km)
{
    float seconds = (float)(int32_t)(now - a->report_ms) / 1000.0f;
    if (seconds < 0)
        seconds = 0;
    if (seconds > 60) /* no report for a minute: stop guessing */
        seconds = 60;
    float km = a->speed_kmh * seconds / 3600.0f, rad = a->track_deg * (float)M_PI / 180.0f;
    *east_km = a->east_km + km * sinf(rad);
    *north_km = a->north_km + km * cosf(rad);
}
