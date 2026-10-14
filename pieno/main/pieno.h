#ifndef PIENO_H
#define PIENO_H

/*
 * Pieno: the cheapest fuel around you, from the ministry's open data.
 *
 *   feed.c    once a day (after 8:00, when MIMIT publishes) downloads the two
 *             CSV files (~7.5 MB) in a stream, keeps the stations within the
 *             radius, saves a cache (opens instantly, works offline), logs the
 *             area's daily min/median/max and every price change it sees.
 *   ui.c      the watch: best price now, ranking, station, trend, dashboard.
 *   web.c     the dashboard: map, table, trends, trip calculator, settings.
 *
 * Ranking by "costo reale": a cheaper station far away costs the fuel burnt
 * to get there and back (fuel.c pn_real_price()).
 *
 * Thread model: the worker owns downloads; everyone else reads the data
 * under pn_lock()/pn_unlock() (short: copy what you need).
 */

#include "fuel.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#define PN_MAX_STATIONS 700
#define PN_MAX_EXTRAS   6
#define PN_HISTORY_DAYS 400

typedef struct
{
    char product[24];
    float price;
    bool self;
} pn_extra_t;

typedef struct
{
    uint32_t id;
    char brand[24];
    char name[48];
    char address[64];
    char town[32];
    char province[4];
    float lat;
    float lon;
    float km;                          /* from home */
    float price[PN_FUELS][2];          /* [fuel][PN_SERVED/PN_SELF], 0 = not sold */
    int64_t when[PN_FUELS][2];         /* local seconds of the communication */
    float previous[PN_FUELS][2];       /* the price before the last change seen, 0 = none */
    uint8_t extra_count;
    pn_extra_t extra[PN_MAX_EXTRAS];   /* premium products, for the dashboard */
} pn_station_t;

typedef struct
{
    float lat;
    float lon;
    char place[40];
    uint8_t radius_km;      /* 3..30 */
    uint8_t fuel;           /* PN_BENZINA... */
    uint8_t self;           /* PN_SELF / PN_SERVED */
    uint8_t real_cost;      /* rank by real cost (detour included) */
    float litres;           /* a usual fill */
    float km_per_litre;     /* the car's consumption */
    float target;           /* "good price" threshold, 0 = none */
    uint8_t max_age_days;   /* older prices are "not confirmed": out of ranking and statistics (0 = keep all) */
} pn_settings_t;

typedef enum
{
    PN_IDLE,
    PN_WAITING_WIFI,
    PN_STATIONS,            /* downloading the station list */
    PN_PRICES,              /* downloading the prices */
    PN_FAILED,
} pn_phase_t;

typedef struct
{
    pn_phase_t phase;
    int percent;
    char message[72];
    char extraction[12];    /* "2026-10-12": the date of the data */
    int64_t fetched;        /* unix time of the last good download, 0 = never */
    int count;              /* stations in the radius */
    uint32_t revision;      /* changes whenever the data or settings change */
} pn_status_t;

typedef struct
{
    int32_t day;            /* days since 1970 */
    uint8_t fuel;
    uint8_t self;
    uint16_t count;
    float min;
    float median;
    float max;
} pn_day_t;

esp_err_t pn_start(void);
/* Internal app closed: the worker ends (after a download in progress). */
void pn_stop(void);
void pn_refresh(void);
void pn_status(pn_status_t *out);
void pn_settings(pn_settings_t *out);
/* Applies at once; a new place or radius downloads again. */
void pn_set_settings(const pn_settings_t *settings);

/* The station table: valid between lock and unlock. */
void pn_lock(void);
void pn_unlock(void);
const pn_station_t *pn_stations(int *count);

/* The price is sold and recent enough for the settings. */
bool pn_price_valid(const pn_station_t *station, int fuel, int self, const pn_settings_t *settings);

/* Price used for ranking: real cost or plain price; 0 if not sold. Locked by the caller. */
float pn_rank_price(const pn_station_t *station, int fuel, int self, const pn_settings_t *settings);
/* Station indices by rank (cheapest first), stations not selling the fuel left out. Locked. */
int pn_rank(int fuel, int self, const pn_settings_t *settings, uint16_t *order, int max);
/* Area statistics now (valid prices only). Locked. */
bool pn_area(int fuel, int self, const pn_settings_t *settings, float *min, float *median, float *max, int *count);

/* Daily area history (oldest first) for one fuel/mode. */
int pn_history(int fuel, int self, pn_day_t *out, int max);
/* Price changes of one station: lines "day,fuel,self,price" into a text buffer. */
int pn_changes(uint32_t id, char *out, int size);

/* Local now in the same scale as pn_station_t.when (seconds, no zone). */
int64_t pn_local_now(void);

/* UI and dashboard. */
void pieno_ui_create(lv_obj_t *screen);
bool pieno_ui_back(void);
/* Internal app closed (widgets already deleted by the app manager). */
void pieno_ui_destroy(void);
esp_err_t pieno_web_start(void);

extern const char *const pn_fuel_names[PN_FUELS];

#endif
