/*
 * Radar - the real rain radar around you, animated, with a nowcast.
 *
 *   feed.c     Wi-Fi worker: dark base map (OpenStreetMap tiles, darkened on
 *              the watch), radar frames of the last two hours (RainViewer),
 *              15-minute precipitation for the next two hours (Open-Meteo),
 *              arrival estimate from the radar itself (nearest rain, then and
 *              now)
 *   compose.c  pure C: tile maths, palette, darkening, frame composition,
 *              nearest-rain search (host-tested)
 *   ui.c       full-screen map with the loop, verdict card, details page
 *
 * Memory (PSRAM): base map RGB565 + one byte per pixel per radar frame.
 */
#ifndef RADAR_H
#define RADAR_H

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#define RADAR_FRAMES 13
#define RADAR_NOWCAST 8 /* 15-minute slots: next two hours */
#define RADAR_HOURS 12

typedef enum
{
    RADAR_ZOOM_NEAR,   /* ~170 km across: base z8, radar z7 x2 */
    RADAR_ZOOM_REGION, /* ~340 km: base z7, radar z7 */
    RADAR_ZOOM_COUNTRY,/* ~700 km: base z6, radar z6 */
    RADAR_ZOOM_COUNT,
} radar_zoom_t;

typedef struct
{
    bool located, online, loading;
    int zoom;
    int frames;           /* frames ready, newest last */
    int progress;         /* 0..100 while loading */
    uint32_t revision;    /* changes when frames or nowcast change */
    int64_t frame_time[RADAR_FRAMES];
    int64_t newest;       /* unix time of the newest frame */
    float km_per_pixel;   /* on screen */
    char message[64];
    /* Nowcast (Open-Meteo) */
    bool nowcast_valid;
    int64_t nowcast_start; /* unix time of slot 0 */
    float rain_mm[RADAR_NOWCAST];  /* per 15 minutes */
    int hour_probability[RADAR_HOURS];
    int hour_start;       /* local hour of hour_probability[0] */
    /* Radar-derived arrival */
    bool cell_valid;
    float cell_km;        /* nearest rain now */
    float cell_bearing;   /* degrees from north, where it is */
    float cell_speed_kmh; /* approach speed (>0 = coming) */
    float rain_here;      /* radar intensity 0..1 at your position */
} radar_status_t;

esp_err_t radar_start(void);
void radar_status(radar_status_t *out);
void radar_set_zoom(radar_zoom_t zoom);
void radar_refresh_now(void);
/* QA console only: show another place. */
void radar_test_location(float lat, float lon);

/* Compose frame i (0 = oldest ready) over the base map into out (RGB565, screen size).
 * Returns false if the frame is not ready. Safe from the lvgl task. */
bool radar_compose(int frame, uint16_t *out);
/* Base map only (while frames load). */
bool radar_base(uint16_t *out);

#endif
