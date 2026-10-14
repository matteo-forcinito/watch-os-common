/*
 * Radar feed (see radar.h). One worker owns Wi-Fi and HTTP:
 *
 *   base map   tile.openstreetmap.org (identified User-Agent, a handful of
 *              tiles per view, darkened by compose.c)
 *   radar      api.rainviewer.com/public/weather-maps.json -> 13 frames of the
 *              last two hours, tiles from tilecache.rainviewer.com (zoom <= 7)
 *   nowcast    api.open-meteo.com minutely_15 precipitation + hourly probability
 *
 * Tiles are fetched over kept-alive HTTPS connections (one TLS handshake per
 * host, not per tile). The newest frame comes first so the map is useful at
 * once; older frames fill the loop. Every 10 minutes only new frames are
 * fetched. Arrival estimate: distance to the nearest rain now and 30 minutes
 * ago (radar frames), speed = how much closer it came.
 */
#include "radar.h"
#include "compose.h"

#include "cJSON.h"
#include "core/settings.h"
#include "core/sys.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hardware/board.h"
#include "libs/lodepng/lodepng.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "services/wifi.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "radar";
/* Per-request timing: esp_log_level_set("radar", ESP_LOG_DEBUG) in QA. */

#define TILE_BYTES (96 * 1024)
#define JSON_BYTES (16 * 1024)
#define REFRESH_MS (10 * 60 * 1000)
#define USER_AGENT "WatchOS-Radar/1.0 (personal smartwatch; github.com/matteo-forcinito)"

typedef struct
{
    int base_zoom, radar_zoom;
} zoom_t;

static const zoom_t s_zooms[RADAR_ZOOM_COUNT] = {{8, 7}, {7, 7}, {6, 6}};

static SemaphoreHandle_t s_lock;
static radar_status_t s_status;
static radar_palette_t *s_palette;
static uint16_t *s_base;
static uint8_t *s_pool[RADAR_FRAMES];   /* every frame buffer */
static uint8_t *s_frames[RADAR_FRAMES]; /* published, complete, oldest first */
static int s_width, s_height, s_frame_slots;
static atomic_int s_zoom = RADAR_ZOOM_NEAR;
static float s_test_lat, s_test_lon; /* QA: another place than the weather location */
static atomic_bool s_refresh;
static TaskHandle_t s_task;
static uint8_t *s_download;
static char *s_json;

/* ------------------------------------------------------------ HTTP (kept alive per host) */

typedef struct
{
    esp_http_client_handle_t client;
    char host[64];
    uint8_t *buffer;
    size_t capacity, length;
    bool overflow;
} http_t;

static http_t s_http;

static esp_err_t on_http(esp_http_client_event_t *event)
{
    http_t *h = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0)
    {
        if (h->length + (size_t)event->data_len >= h->capacity)
            h->overflow = true;
        else
        {
            memcpy(h->buffer + h->length, event->data, event->data_len);
            h->length += event->data_len;
        }
    }
    return ESP_OK;
}

static void host_of(const char *url, char *host, size_t size)
{
    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    size_t n = strcspn(start, "/:");
    snprintf(host, size, "%.*s", (int)(n < size ? n : size - 1), start);
}

/* GET into buffer; returns the HTTP status (0 = network error). */
static int http_get(const char *url, uint8_t *buffer, size_t capacity, size_t *length)
{
    char host[64];
    host_of(url, host, sizeof(host));
    if (s_http.client == NULL || strcmp(host, s_http.host) != 0)
    {
        if (s_http.client)
            esp_http_client_cleanup(s_http.client);
        esp_http_client_config_t config = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 12000,
            .keep_alive_enable = true,
            .user_agent = USER_AGENT,
            .event_handler = on_http,
            .user_data = &s_http,
            .buffer_size = 2048,
        };
        s_http.client = esp_http_client_init(&config);
        snprintf(s_http.host, sizeof(s_http.host), "%s", host);
        if (s_http.client == NULL)
            return 0;
    }
    else
        esp_http_client_set_url(s_http.client, url);
    s_http.buffer = buffer;
    s_http.capacity = capacity;
    s_http.length = 0;
    s_http.overflow = false;
    int64_t started = esp_timer_get_time();
    esp_err_t error = esp_http_client_perform(s_http.client);
#if CONFIG_WATCH_DEV_CONSOLE
    ESP_LOGI(TAG, "GET %s: %s, %u bytes, %d ms", url, esp_err_to_name(error), (unsigned)s_http.length,
             (int)((esp_timer_get_time() - started) / 1000));
#else
    (void)started;
#endif
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "%s: %s", host, esp_err_to_name(error));
        esp_http_client_cleanup(s_http.client);
        s_http.client = NULL;
        return 0;
    }
    *length = s_http.length;
    buffer[s_http.length] = 0;
    int status = esp_http_client_get_status_code(s_http.client);
    return s_http.overflow ? 0 : status;
}

/* ------------------------------------------------------------ geometry */

static void world_pixel(double lat, double lon, int zoom, double *x, double *y)
{
    double n = 256.0 * (double)(1 << zoom), s = sin(lat * M_PI / 180.0);
    *x = (lon + 180.0) / 360.0 * n;
    *y = (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * n;
}

typedef struct
{
    int base_zoom, radar_zoom, shift;
    int32_t ox, oy; /* screen origin in base-zoom global pixels */
} view_t;

static view_t make_view(double lat, double lon, int zoom)
{
    view_t v = {.base_zoom = s_zooms[zoom].base_zoom, .radar_zoom = s_zooms[zoom].radar_zoom};
    v.shift = v.base_zoom - v.radar_zoom;
    double x, y;
    world_pixel(lat, lon, v.base_zoom, &x, &y);
    v.ox = (int32_t)(x - s_width / 2);
    v.oy = (int32_t)(y - s_height / 2);
    return v;
}

static void set_message(const char *text)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.message, sizeof(s_status.message), "%s", text);
    s_status.revision++;
    xSemaphoreGive(s_lock);
}

static void set_progress(int percent)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.progress = percent;
    xSemaphoreGive(s_lock);
}

/* Decode a downloaded PNG. LVGL's lodepng returns an lv_draw_buf_t (RGBA bytes in
 * ->data, stride 4 * 256), not a raw array: free it with lv_draw_buf_destroy(). */
static lv_draw_buf_t *decode_png(size_t length)
{
    unsigned char *out = NULL;
    unsigned w = 0, h = 0;
    unsigned error = lodepng_decode32(&out, &w, &h, s_download, length);
    lv_draw_buf_t *buf = (lv_draw_buf_t *)out;
    if (error != 0 || buf == NULL || w != 256 || h != 256 || buf->header.stride != 256 * 4)
    {
        if (buf)
            lv_draw_buf_destroy(buf);
        return NULL;
    }
    return buf;
}

/* ------------------------------------------------------------ base map */

static bool build_base(const view_t *v, uint16_t *base)
{
    int32_t x0 = v->ox >> 8, x1 = (v->ox + s_width - 1) >> 8, y0 = v->oy >> 8, y1 = (v->oy + s_height - 1) >> 8;
    int total = (x1 - x0 + 1) * (y1 - y0 + 1), done = 0;
    for (int i = 0; i < s_width * s_height; i++)
        base[i] = 0x0841; /* dark blue-grey while tiles arrive */
    for (int32_t ty = y0; ty <= y1; ty++)
        for (int32_t tx = x0; tx <= x1; tx++)
        {
            char url[96];
            size_t length = 0;
            snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%ld/%ld.png", v->base_zoom, (long)tx, (long)ty);
            int status = http_get(url, s_download, TILE_BYTES, &length);
            if (status != 200)
            {
                ESP_LOGW(TAG, "Map tile %s: HTTP %d", url, status);
                return false;
            }
            lv_draw_buf_t *tile = decode_png(length);
            if (tile == NULL)
            {
                ESP_LOGW(TAG, "Map tile %s: not a 256 px PNG (%u bytes)", url, (unsigned)length);
                return false;
            }
            radar_blit_base(base, s_width, s_height, v->ox, v->oy, tile->data, tx, ty);
            lv_draw_buf_destroy(tile);
            set_progress(++done * 30 / total);
        }
    return true;
}

/* ------------------------------------------------------------ radar frames */

static bool build_frame(const view_t *v, const char *host, const char *path, uint8_t *frame)
{
    int32_t rx0 = (v->ox >> v->shift) >> 8, rx1 = ((v->ox + s_width - 1) >> v->shift) >> 8;
    int32_t ry0 = (v->oy >> v->shift) >> 8, ry1 = ((v->oy + s_height - 1) >> v->shift) >> 8;
    memset(frame, 0, (size_t)s_width * s_height);
    for (int32_t ty = ry0; ty <= ry1; ty++)
        for (int32_t tx = rx0; tx <= rx1; tx++)
        {
            char url[160];
            size_t length = 0;
            /* 256 px, colour scheme 2 (universal blue), smoothing on, snow shown. */
            snprintf(url, sizeof(url), "%s%s/256/%d/%ld/%ld/2/1_1.png", host, path, v->radar_zoom, (long)tx, (long)ty);
            int status = http_get(url, s_download, TILE_BYTES, &length);
            if (status != 200)
            {
                ESP_LOGW(TAG, "Radar tile %s: HTTP %d", url, status);
                return false;
            }
            lv_draw_buf_t *tile = decode_png(length);
            if (tile == NULL)
            {
                ESP_LOGW(TAG, "Radar tile %s: not a 256 px PNG (%u bytes)", url, (unsigned)length);
                return false;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY); /* the palette is shared with the UI */
            radar_blit_radar(frame, s_width, s_height, v->ox, v->oy, v->shift, tile->data, tx, ty, s_palette);
            xSemaphoreGive(s_lock);
            lv_draw_buf_destroy(tile);
        }
    return true;
}

/* Arrival estimate from the newest frame and the one 30 minutes earlier. */
static void estimate_arrival(float km_per_pixel)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_status.frames;
    float d_now = 0, b_now = 0, d_then = 0, b_then = 0;
    bool now = n > 0 && radar_nearest_rain(s_frames[n - 1], s_palette, s_width, s_height, s_width / 2, s_height / 2,
                                           90, &d_now, &b_now);
    bool then = n > 3 && radar_nearest_rain(s_frames[n - 4], s_palette, s_width, s_height, s_width / 2,
                                            s_height / 2, 90, &d_then, &b_then);
    s_status.cell_valid = now;
    s_status.cell_km = d_now * km_per_pixel;
    s_status.cell_bearing = b_now;
    s_status.cell_speed_kmh = now && then ? (d_then - d_now) * km_per_pixel * 2.0f : 0; /* per 30 min -> per hour */
    s_status.rain_here = n > 0 ? radar_intensity_at(s_frames[n - 1], s_palette, s_width, s_height, s_width / 2,
                                                    s_height / 2, 3)
                               : 0;
    s_status.revision++;
    xSemaphoreGive(s_lock);
}

/* Publish the complete frames (oldest first). Buffers being written are never published. */
static void publish(int count, uint8_t *const *slot, const int64_t *times, const bool *have, int progress)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < count; i++)
        if (have[i])
        {
            s_frames[n] = slot[i];
            s_status.frame_time[n] = times[i];
            n++;
        }
    s_status.frames = n;
    s_status.newest = n ? s_status.frame_time[n - 1] : 0;
    s_status.progress = progress;
    s_status.revision++;
    xSemaphoreGive(s_lock);
}

/* Fetch the frame list; reuse frames we already have, fetch the others (newest first). */
static bool update_frames(const view_t *v, bool rebuild)
{
    size_t length = 0;
    if (http_get("https://api.rainviewer.com/public/weather-maps.json", (uint8_t *)s_json, JSON_BYTES, &length) != 200)
        return false;
    cJSON *root = cJSON_Parse(s_json);
    const cJSON *host = cJSON_GetObjectItem(root, "host");
    const cJSON *past = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "radar"), "past");
    if (!cJSON_IsString(host) || !cJSON_IsArray(past))
    {
        cJSON_Delete(root);
        return false;
    }
    int available = cJSON_GetArraySize(past);
    int count = available < s_frame_slots ? available : s_frame_slots;
    int64_t times[RADAR_FRAMES];
    char paths[RADAR_FRAMES][48];
    for (int i = 0; i < count; i++)
    {
        const cJSON *item = cJSON_GetArrayItem(past, available - count + i);
        const cJSON *time_item = cJSON_GetObjectItem(item, "time"), *path = cJSON_GetObjectItem(item, "path");
        times[i] = cJSON_IsNumber(time_item) ? (int64_t)time_item->valuedouble : 0;
        snprintf(paths[i], sizeof(paths[i]), "%s", cJSON_IsString(path) ? path->valuestring : "");
    }
    char host_text[64];
    snprintf(host_text, sizeof(host_text), "%s", host->valuestring);
    cJSON_Delete(root);

    /* Reuse published frames with the same time; the other pool buffers get the new ones. */
    uint8_t *old[RADAR_FRAMES];
    int64_t old_time[RADAR_FRAMES];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int old_count = s_status.frames;
    memcpy(old, s_frames, sizeof(old));
    memcpy(old_time, s_status.frame_time, sizeof(old_time));
    xSemaphoreGive(s_lock);
    uint8_t *slot[RADAR_FRAMES] = {0};
    bool have[RADAR_FRAMES] = {false}, busy[RADAR_FRAMES] = {false};
    for (int i = 0; i < count && !rebuild; i++)
        for (int k = 0; k < old_count && !have[i]; k++)
            if (old_time[k] == times[i])
            {
                slot[i] = old[k];
                have[i] = true;
                for (int b = 0; b < s_frame_slots; b++)
                    busy[b] = busy[b] || s_pool[b] == old[k];
            }
    int missing = 0;
    for (int i = 0, b = 0; i < count; i++)
    {
        if (have[i])
            continue;
        while (b < s_frame_slots && busy[b])
            b++;
        if (b == s_frame_slots)
            return false; /* cannot happen: count <= pool size */
        slot[i] = s_pool[b];
        busy[b] = true;
        missing++;
    }
    /* Only complete frames stay visible while the others are rewritten. */
    int fetched = 0;
    publish(count, slot, times, have, 30);
    for (int i = count - 1; i >= 0; i--)
    {
        if (have[i])
            continue;
        if (!build_frame(v, host_text, paths[i], slot[i]))
            return false;
        have[i] = true;
        fetched++;
        publish(count, slot, times, have, 30 + 70 * fetched / missing);
    }
    return true;
}

/* ------------------------------------------------------------ nowcast */

static void update_nowcast(double lat, double lon)
{
    char url[320];
    size_t length = 0;
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&minutely_15=precipitation"
             "&forecast_minutely_15=%d&hourly=precipitation_probability&forecast_hours=%d&timezone=auto"
             "&timeformat=unixtime",
             lat, lon, RADAR_NOWCAST, RADAR_HOURS);
    if (http_get(url, (uint8_t *)s_json, JSON_BYTES, &length) != 200)
        return;
    cJSON *root = cJSON_Parse(s_json);
    const cJSON *minutely = cJSON_GetObjectItem(root, "minutely_15"), *hourly = cJSON_GetObjectItem(root, "hourly");
    const cJSON *times = cJSON_GetObjectItem(minutely, "time"), *rain = cJSON_GetObjectItem(minutely, "precipitation");
    const cJSON *hour_times = cJSON_GetObjectItem(hourly, "time");
    const cJSON *probability = cJSON_GetObjectItem(hourly, "precipitation_probability");
    const cJSON *offset = cJSON_GetObjectItem(root, "utc_offset_seconds");
    if (cJSON_IsArray(times) && cJSON_IsArray(rain) && cJSON_GetArraySize(rain) >= RADAR_NOWCAST)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.nowcast_valid = true;
        s_status.nowcast_start = (int64_t)cJSON_GetArrayItem(times, 0)->valuedouble;
        for (int i = 0; i < RADAR_NOWCAST; i++)
        {
            const cJSON *value = cJSON_GetArrayItem(rain, i);
            s_status.rain_mm[i] = cJSON_IsNumber(value) ? (float)value->valuedouble : 0;
        }
        if (cJSON_IsArray(probability) && cJSON_IsArray(hour_times) && cJSON_GetArraySize(probability) >= RADAR_HOURS)
        {
            for (int i = 0; i < RADAR_HOURS; i++)
            {
                const cJSON *value = cJSON_GetArrayItem(probability, i);
                s_status.hour_probability[i] = cJSON_IsNumber(value) ? value->valueint : 0;
            }
            int64_t first = (int64_t)cJSON_GetArrayItem(hour_times, 0)->valuedouble;
            int64_t local = first + (cJSON_IsNumber(offset) ? offset->valueint : 0);
            s_status.hour_start = (int)((local / 3600) % 24);
        }
        s_status.revision++;
        xSemaphoreGive(s_lock);
    }
    cJSON_Delete(root);
}

/* ------------------------------------------------------------ worker */

static void radar_task(void *arg)
{
    (void)arg;
    bool online = false;
    int built_zoom = -1, tried_zoom = -1;
    float built_lat = 0, built_lon = 0;
    uint32_t next_refresh = 0;
    for (;;)
    {
        const settings_t *settings = settings_get();
        float lat = settings->weather_lat, lon = settings->weather_lon;
        if (s_test_lat != 0 || s_test_lon != 0)
        {
            lat = s_test_lat;
            lon = s_test_lon;
        }
        bool located = !(lat == 0 && lon == 0);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.located = located;
        xSemaphoreGive(s_lock);
        if (!located)
        {
            set_message("Imposta la tua citta' nel Meteo");
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }
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
        }
        int zoom = atomic_load(&s_zoom);
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        bool rebuild = zoom != built_zoom || lat != built_lat || lon != built_lon;
        bool asked = atomic_exchange(&s_refresh, false) || zoom != tried_zoom;
        bool due = asked || (int32_t)(now - next_refresh) >= 0;
        if (!due)
        {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        tried_zoom = zoom;
        view_t view = make_view(lat, lon, zoom);
        float km_per_pixel = (float)(156543.03 * cos(lat * M_PI / 180.0) / (double)(1 << view.base_zoom) / 1000.0);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.loading = true;
        s_status.zoom = zoom;
        s_status.km_per_pixel = km_per_pixel;
        if (rebuild)
            s_status.frames = 0;
        s_status.revision++;
        xSemaphoreGive(s_lock);
        bool ok = true;
        ESP_LOGI(TAG, "Update: zoom %d, %s", zoom, rebuild ? "new map" : "new frames");
        if (rebuild)
        {
            set_message("Disegno la mappa...");
            ok = build_base(&view, s_base);
            ESP_LOGI(TAG, "Map %s", ok ? "ready" : "failed");
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.revision++;
            xSemaphoreGive(s_lock);
        }
        if (ok)
        {
            set_message("Scarico il radar...");
            ok = update_frames(&view, rebuild);
            ESP_LOGI(TAG, "Frames %s: %d", ok ? "ready" : "failed", s_status.frames);
        }
        if (ok)
        {
            built_zoom = zoom;
            built_lat = lat;
            built_lon = lon;
            estimate_arrival(km_per_pixel);
            update_nowcast(lat, lon);
            set_message("");
        }
        else
            set_message("Rete lenta o assente, riprovo tra poco");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.loading = false;
        s_status.progress = 100;
        s_status.revision++;
        xSemaphoreGive(s_lock);
        next_refresh = (uint32_t)(esp_timer_get_time() / 1000) + (ok ? REFRESH_MS : 30000);
    }
}

static void *psram_malloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

esp_err_t radar_start(void)
{
    s_width = board_info()->width;
    s_height = board_info()->height;
    size_t pixels = (size_t)s_width * s_height;
    s_lock = xSemaphoreCreateMutex();
    s_palette = heap_caps_calloc(1, sizeof(radar_palette_t), MALLOC_CAP_SPIRAM);
    s_base = heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM);
    s_download = heap_caps_malloc(TILE_BYTES, MALLOC_CAP_SPIRAM);
    s_json = heap_caps_malloc(JSON_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_palette || !s_base || !s_download || !s_json)
        return ESP_ERR_NO_MEM;
    radar_palette_init(s_palette);
    for (size_t i = 0; i < pixels; i++)
        s_base[i] = 0x0841;
    /* As many frames as PSRAM allows (13 = two hours), keeping 1.5 MB free. */
    for (s_frame_slots = 0; s_frame_slots < RADAR_FRAMES; s_frame_slots++)
    {
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < pixels + 1536 * 1024)
            break;
        s_pool[s_frame_slots] = heap_caps_malloc(pixels, MALLOC_CAP_SPIRAM);
        if (s_pool[s_frame_slots] == NULL)
            break;
    }
    ESP_LOGI(TAG, "%d frame buffers of %u bytes", s_frame_slots, (unsigned)pixels);
    if (s_frame_slots < 4)
        return ESP_ERR_NO_MEM;
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
#if CONFIG_WATCH_DEV_CONSOLE
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    snprintf(s_status.message, sizeof(s_status.message), "Avvio...");
    if (!sys_task_create(radar_task, "radar_feed", 8192, NULL, 4, tskNO_AFFINITY))
        return ESP_ERR_NO_MEM;
    s_task = xTaskGetHandle("radar_feed");
    return ESP_OK;
}

void radar_status(radar_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

void radar_set_zoom(radar_zoom_t zoom)
{
    if (zoom >= RADAR_ZOOM_COUNT)
        return;
    atomic_store(&s_zoom, zoom);
    if (s_task)
        xTaskNotifyGive(s_task);
}

void radar_test_location(float lat, float lon)
{
    s_test_lat = lat;
    s_test_lon = lon;
    radar_refresh_now();
}

void radar_refresh_now(void)
{
    atomic_store(&s_refresh, true);
    if (s_task)
        xTaskNotifyGive(s_task);
}

bool radar_compose(int frame, uint16_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = frame >= 0 && frame < s_status.frames;
    if (ok)
        radar_compose_frame(s_base, s_frames[frame], s_palette, out, s_width * s_height);
    xSemaphoreGive(s_lock);
    return ok;
}

bool radar_base(uint16_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, s_base, (size_t)s_width * s_height * 2);
    xSemaphoreGive(s_lock);
    return true;
}
