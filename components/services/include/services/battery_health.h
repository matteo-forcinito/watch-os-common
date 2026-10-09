#ifndef SERVICES_BATTERY_HEALTH_H
#define SERVICES_BATTERY_HEALTH_H

/*
 * Battery health: an ESTIMATE. None of the supported boards has a fuel gauge
 * that measures capacity, so health is the worst of three indicators:
 *
 *   cycles      every % used while not charging is summed: 100 % = 1 cycle.
 *               A LiPo loses ~20 % of its capacity in ~400 cycles.
 *   age         calendar ageing, ~3 % a year, from the first day this
 *               firmware saw the battery.
 *   resistance  the "health test": the voltage drop when a fixed load (screen
 *               white at full brightness) is switched on. An ageing battery
 *               drops more; compared with the first test on this battery.
 *
 * Samples are taken every 2 minutes in the sys worker (no hardware access:
 * the power manager already publishes STATE_BATTERY / STATE_CHARGING) and
 * saved in NVS. Pure model: battery_health_model() (host-testable).
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define BATTERY_UNKNOWN  (-1)

typedef struct
{
    int percent;           /* 0..100, BATTERY_UNKNOWN */
    int mv;                /* last voltage, 0 if unknown */
    bool charging;
    bool usb;
    bool full;             /* on the cable and charged */

    int health;            /* 0..100 = worst of the three, BATTERY_UNKNOWN */
    int health_cycles;
    int health_age;        /* BATTERY_UNKNOWN until the clock is set */
    int health_resistance; /* BATTERY_UNKNOWN until two tests */

    uint32_t cycles_x10;   /* tenths of full cycles */
    int days;              /* since first seen, BATTERY_UNKNOWN without clock */
    int64_t first_seen;    /* unix time, 0 if unknown */
    int tests;             /* resistance tests stored */
    int resistance_change; /* % vs the first test (e.g. +12), only if tests >= 2 */
    int minutes_left;      /* estimated, BATTERY_UNKNOWN while learning */
} battery_report_t;

/* Load the history and start sampling. Before apps_init(). */
void battery_health_init(void);

/* Current report (any task; no hardware access). */
void battery_health_get(battery_report_t *out);

/*
 * Store one resistance test: battery voltage without and with the test load
 * at `percent` charge. Returns ESP_ERR_INVALID_ARG if the drop is too small
 * to mean anything.
 */
esp_err_t battery_health_add_test(int idle_mv, int load_mv, int percent);

/* A new battery: forget cycles, age and tests. */
void battery_health_reset(void);

/* ---- pure model (no ESP-IDF), exposed for tests */

/* Health % from each indicator; pass BATTERY_UNKNOWN for missing inputs. */
int battery_health_from_cycles(uint32_t cycles_x10);
int battery_health_from_age(int days);
int battery_health_from_resistance(int first_drop_mv, int last_drop_mv);

#endif
