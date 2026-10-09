#ifndef SERVICES_WEATHER_H
#define SERVICES_WEATHER_H

/*
 * Weather for the city in the settings.
 *
 * Two sources:
 *   - the phone (Gadgetbridge sends the weather over Bluetooth: no Wi-Fi
 *     needed, the cheapest option for the battery);
 *   - Open-Meteo over HTTPS (free, no API key), fetched only on demand:
 *     when the weather app opens or the data is older than 2 hours.
 *
 * Changes bump STATE_WEATHER_VERSION.
 */

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#define WEATHER_DAYS 4

typedef struct
{
    bool valid;
    bool from_phone;
    time_t updated;
    char city[40];
    float temp;
    float temp_min;
    float temp_max;
    float wind_kmh;
    int humidity;
    int code;            /* WMO weather code (see ui_weather_icon) */
    bool is_day;
    struct
    {
        int code;
        float temp_min;
        float temp_max;
    } days[WEATHER_DAYS];
    int day_count;
} weather_t;

void weather_init(void);

/* Fetch in the background if older than `max_age_s` (0 = always). */
void weather_refresh(int max_age_s);

bool weather_is_updating(void);

bool weather_get(weather_t *out);

/* Data from the phone (temperatures in °C). */
void weather_set_from_phone(const weather_t *w);

/* Look up a city (geocoding) and save it in the settings. Blocking (Wi-Fi). */
esp_err_t weather_set_city(const char *name);

#endif
