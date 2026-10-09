#include "services/weather.h"

#include "services/http_stream.h"
#include "core/sys.h"

#include "core/settings.h"
#include "core/state.h"
#include "services/wifi.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "weather";

#define TEXT_MAX (8 * 1024)

static weather_t s_weather;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_updating = false;

static void url_encode(const char *in, char *out, size_t size)
{
    size_t used = 0;

    for (; *in != '\0' && used + 4 < size; in++)
    {
        unsigned char c = (unsigned char)*in;

        if (isalnum(c) || c == '-' || c == '_' || c == '.')
        {
            out[used++] = (char)c;
        }
        else
        {
            used += (size_t)snprintf(out + used, size - used, "%%%02X", c);
        }
    }

    out[used] = '\0';
}

static double number(const cJSON *object, const char *key)
{
    return cJSON_GetNumberValue(cJSON_GetObjectItem(object, key));
}

static bool parse_forecast(const char *text, weather_t *w)
{
    cJSON *root = cJSON_Parse(text);

    if (root == NULL)
    {
        return false;
    }

    const cJSON *current = cJSON_GetObjectItem(root, "current");
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    bool ok = current != NULL;

    if (ok)
    {
        w->temp = (float)number(current, "temperature_2m");
        w->humidity = (int)number(current, "relative_humidity_2m");
        w->code = (int)number(current, "weather_code");
        w->wind_kmh = (float)number(current, "wind_speed_10m");
        w->is_day = number(current, "is_day") > 0.5;
    }

    if (ok && daily != NULL)
    {
        const cJSON *codes = cJSON_GetObjectItem(daily, "weather_code");
        const cJSON *max = cJSON_GetObjectItem(daily, "temperature_2m_max");
        const cJSON *min = cJSON_GetObjectItem(daily, "temperature_2m_min");
        int count = cJSON_GetArraySize(codes);

        w->day_count = count > WEATHER_DAYS ? WEATHER_DAYS : count;

        for (int i = 0; i < w->day_count; i++)
        {
            w->days[i].code = (int)cJSON_GetNumberValue(cJSON_GetArrayItem(codes, i));
            w->days[i].temp_max = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(max, i));
            w->days[i].temp_min = (float)cJSON_GetNumberValue(cJSON_GetArrayItem(min, i));
        }

        if (w->day_count > 0)
        {
            w->temp_min = w->days[0].temp_min;
            w->temp_max = w->days[0].temp_max;
        }
    }

    cJSON_Delete(root);

    return ok;
}

static void fetch_task(void *arg)
{
    (void)arg;

    char *text = malloc(TEXT_MAX);
    char *url = malloc(HTTP_STREAM_URL_MAX);

    if (text != NULL && url != NULL && wifi_acquire(15000) == ESP_OK)
    {
        const settings_t *s = settings_get();
        snprintf(url, HTTP_STREAM_URL_MAX,
                 "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                 "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
                 "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=%d",
                 (double)s->weather_lat, (double)s->weather_lon, WEATHER_DAYS);

        weather_t w = {0};

        if (http_stream_get_text(url, text, TEXT_MAX, NULL, 0) == ESP_OK && parse_forecast(text, &w))
        {
            w.valid = true;
            w.updated = time(NULL);
            snprintf(w.city, sizeof(w.city), "%s", s->weather_city);

            portENTER_CRITICAL(&s_lock);
            s_weather = w;
            portEXIT_CRITICAL(&s_lock);

            state_bump(STATE_WEATHER_VERSION);
            ESP_LOGI(TAG, "%s: %.1f C, code %d", w.city, (double)w.temp, w.code);
        }
        else
        {
            ESP_LOGW(TAG, "Update failed");
        }

        wifi_release();
    }

    free(text);
    free(url);
    s_updating = false;
    state_bump(STATE_WEATHER_VERSION);
    vTaskDelete(NULL);
}

void weather_init(void)
{
    memset(&s_weather, 0, sizeof(s_weather));
}

void weather_refresh(int max_age_s)
{
    if (s_updating)
    {
        return;
    }

    if (max_age_s > 0 && s_weather.valid && time(NULL) - s_weather.updated < max_age_s)
    {
        return;
    }

    if (!wifi_known_any())
    {
        return;
    }

    s_updating = true;
    state_bump(STATE_WEATHER_VERSION);

    if (!sys_task_create(fetch_task, "weather", 6144, NULL, 3, SYS_CORE_ANY))
    {
        s_updating = false;
    }
}

bool weather_is_updating(void)
{
    return s_updating;
}

bool weather_get(weather_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_weather;
    portEXIT_CRITICAL(&s_lock);

    return out->valid;
}

void weather_set_from_phone(const weather_t *w)
{
    portENTER_CRITICAL(&s_lock);
    s_weather = *w;
    s_weather.valid = true;
    s_weather.from_phone = true;
    s_weather.updated = time(NULL);
    portEXIT_CRITICAL(&s_lock);

    state_bump(STATE_WEATHER_VERSION);
}

esp_err_t weather_set_city(const char *name)
{
    char *text = malloc(TEXT_MAX);
    char *url = malloc(HTTP_STREAM_URL_MAX);
    char encoded[120];
    esp_err_t err = ESP_ERR_NO_MEM;

    if (text == NULL || url == NULL)
    {
        goto done;
    }

    err = wifi_acquire(15000);

    if (err != ESP_OK)
    {
        goto done;
    }

    url_encode(name, encoded, sizeof(encoded));
    snprintf(url, HTTP_STREAM_URL_MAX,
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=it&format=json", encoded);

    err = http_stream_get_text(url, text, TEXT_MAX, NULL, 0);
    wifi_release();

    if (err != ESP_OK)
    {
        goto done;
    }

    cJSON *root = cJSON_Parse(text);
    const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "results"), 0);
    err = ESP_ERR_NOT_FOUND;

    if (first != NULL)
    {
        settings_t s = *settings_get();
        const char *found = cJSON_GetStringValue(cJSON_GetObjectItem(first, "name"));
        snprintf(s.weather_city, sizeof(s.weather_city), "%s", found != NULL ? found : name);
        s.weather_lat = (float)number(first, "latitude");
        s.weather_lon = (float)number(first, "longitude");
        settings_save(&s);
        err = ESP_OK;
        ESP_LOGI(TAG, "City: %s (%.3f, %.3f)", s.weather_city, (double)s.weather_lat, (double)s.weather_lon);
    }

    cJSON_Delete(root);

done:
    free(text);
    free(url);

    if (err == ESP_OK)
    {
        weather_refresh(0);
    }

    return err;
}
