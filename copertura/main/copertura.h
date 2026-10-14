#ifndef COPERTURA_H
#define COPERTURA_H

/*
 * Copertura: how good the Wi-Fi is in every corner of the house.
 *
 *   probe.c   holds the connection; twice a second it reads the signal of
 *             the router we are on, once a second it pings the router
 *             (latency, loss). A "spot" measures 10 seconds in one room,
 *             then scans: other radios of the same network (mesh,
 *             repeaters) and the neighbours (channel advice).
 *   finder.c  hot/cold: walk around, the watch ticks faster as you get
 *             closer to the strongest spot or to the right place for a
 *             repeater (about -62 dBm).
 *   store.c   rooms, floors, measured spots (JSON) and floor plan images.
 *   ui.c      the watch: live gauge, measure a room, finder, results.
 *   web.c     the dashboard: floor plan heatmap, rooms, advice, live chart.
 *
 * Everything shared is read under cp_lock()/cp_unlock().
 */

#include "advice.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#define CP_MAX_POINTS  200
#define CP_MAX_ROOMS   20
#define CP_MAX_FLOORS  3
#define CP_MAX_APS     48
#define CP_ROOM        24
#define CP_LIVE        240     /* 2 samples per second: two minutes */
#define CP_MEASURE_S   10

typedef struct
{
    uint32_t id;
    char room[CP_ROOM];
    uint8_t floor;
    int16_t x;                 /* position on the floor plan in thousandths, -1 = not placed */
    int16_t y;
    int8_t rssi;               /* average */
    int8_t rssi_min;
    int8_t rssi_max;
    uint8_t channel;
    uint8_t bssid[6];          /* the radio we were connected to */
    uint16_t ping;             /* average ms, 0 = no answer */
    uint16_t ping_max;
    uint8_t loss;              /* percent */
    uint8_t radios;            /* radios of the same network heard */
    int8_t alt_rssi;           /* strongest other radio of the same network, 0 = none */
    int64_t when;              /* unix time */
} cp_point_t;

typedef struct
{
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    uint8_t auth;
    bool ours;                 /* same network name as ours */
} cp_ap_t;

typedef struct
{
    bool connected;
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;               /* smoothed */
    int8_t rssi_raw;
    int16_t ping;              /* last, ms; -1 = timeout */
    uint8_t loss;              /* percent over the last 20 pings */
    uint16_t ping_avg;
    char gateway[16];
    char phy[8];               /* "11n", "11ax"... */
} cp_live_t;

typedef enum { CP_IDLE, CP_MEASURING, CP_SCANNING } cp_phase_t;

typedef struct
{
    cp_phase_t phase;
    int percent;
    char room[CP_ROOM];
    uint32_t last_point;       /* id of the last spot saved */
} cp_measure_t;

typedef struct
{
    bool on;
    int mode;                  /* 0 strongest, 1 repeater */
    int heat;                  /* 0..100 */
    int rssi;
    int best;                  /* strongest seen in this search */
    int trend;                 /* -1 worse, 0 steady, 1 better */
} cp_finder_t;

esp_err_t cp_start(void);
/* Internal app closed: the probe releases Wi-Fi, the sound task ends. */
void cp_stop(void);
void cp_lock(void);
void cp_unlock(void);
uint32_t cp_revision(void);

/* Live. */
void cp_live(cp_live_t *out);
/* Last samples, oldest first: rssi (dBm) and ping (ms, -1 timeout). */
int cp_history(int8_t *rssi, int16_t *ping, int max);

/* Measure a spot in a room (10 s + a scan), on a floor. */
bool cp_measure(const char *room, uint8_t floor);
void cp_measure_status(cp_measure_t *out);

/* Finder. */
void cp_finder_start(int mode);
void cp_finder_stop(void);
void cp_finder_state(cp_finder_t *out);

/* Spots (locked by the caller). */
const cp_point_t *cp_points(int *count);
bool cp_point_place(uint32_t id, int x, int y, int floor);
bool cp_point_delete(uint32_t id);
void cp_points_clear(void);

/* Rooms and floors. */
int cp_rooms(char (*out)[CP_ROOM], int max);
void cp_rooms_set(const char (*rooms)[CP_ROOM], int count);
int cp_floors(char (*out)[CP_ROOM], int max);
void cp_floors_set(const char (*floors)[CP_ROOM], int count);
/* Floor plan image file path for a floor (exists or not). */
bool cp_plan_path(int floor, char *out, size_t size);

/* Neighbourhood (last scan; locked by the caller). */
const cp_ap_t *cp_aps(int *count, int64_t *when);
void cp_scan_now(void);

/* Internal (store.c / probe.c). */
uint32_t cp_point_add(const cp_point_t *point);
void cp_store_load(void);
void cp_store_flush(void);
void cp_changed(bool save);
void cp_sound_init(void);
void cp_sound_stop(void);
void cp_sound_heat(int heat, bool on_target);   /* 0..100, -1 = silent */

/* UI and dashboard. */
void copertura_ui_create(lv_obj_t *screen);
bool copertura_ui_back(void);
/* Internal app closed (widgets already deleted by the app manager). */
void copertura_ui_destroy(void);
esp_err_t copertura_web_start(void);

#endif
