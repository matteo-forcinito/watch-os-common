/*
 * Cielo - the aircraft above you, live.
 *
 *   feed.c   Wi-Fi worker: aircraft around your position from the public
 *            ADS-B network (api.adsb.lol, no key), routes from api.adsbdb.com,
 *            the International Space Station from api.wheretheiss.at
 *   types.c  ICAO type code -> aircraft name
 *   ui.c     sky disc (you at the centre, north up, aircraft gliding between
 *            updates), "look up" card, flight list, flight detail
 *
 * Positions are kept in km east/north of you at the time of the report, with
 * speed and track, so the UI can move every aircraft smoothly between reports.
 */
#ifndef CIELO_H
#define CIELO_H

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#define CIELO_AIRCRAFT 64
#define CIELO_TRAIL 8

typedef struct
{
    char hex[8];
    char callsign[10];
    char registration[12];
    char type[6];
    int32_t altitude_m;   /* barometric; INT32_MIN = on ground / unknown */
    int32_t speed_kmh;
    int32_t vertical_mpm; /* metres per minute, + climbing */
    float track_deg;
    float east_km, north_km; /* relative to you, at report_ms */
    uint32_t report_ms;
    float trail_e[CIELO_TRAIL], trail_n[CIELO_TRAIL]; /* oldest first */
    uint8_t trail_count;
    char squawk[6];
    bool emergency;
} cielo_aircraft_t;

typedef struct
{
    char callsign[10];
    char airline[40];
    char from_code[5], from_city[32];
    char to_code[5], to_city[32];
    bool known;
} cielo_route_t;

typedef struct
{
    bool located;   /* a position is configured */
    bool online;
    bool updating;
    int count;
    uint32_t updated_ms; /* last good report, 0 = never */
    char message[64];
    float lat, lon;
    int radius_km;
    /* International Space Station */
    bool iss_valid;
    float iss_km;        /* ground distance */
    float iss_bearing;   /* degrees from north */
    float iss_altitude_km;
    char iss_over[32];   /* "Sopra l'Atlantico" */
    uint32_t revision;
} cielo_status_t;

esp_err_t cielo_start(void);
void cielo_status(cielo_status_t *out);
int cielo_aircraft(cielo_aircraft_t *out, int max);
bool cielo_find(const char *hex, cielo_aircraft_t *out);
/* Route for a callsign: cached, else queued (returns false until known). */
bool cielo_route(const char *callsign, cielo_route_t *out);
void cielo_set_radius(int km);
void cielo_refresh_now(void);

/* Position of an aircraft now (dead reckoning from the last report). */
void cielo_position_now(const cielo_aircraft_t *a, uint32_t now_ms, float *east_km, float *north_km);

const char *cielo_type_name(const char *code);

/* ui.c (lvgl task) */
#include "lvgl.h"
void cielo_ui_create(lv_obj_t *screen);
bool cielo_ui_back(void);

#endif
