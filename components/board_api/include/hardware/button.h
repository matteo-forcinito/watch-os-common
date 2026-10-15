#ifndef HARDWARE_BUTTON_H
#define HARDWARE_BUTTON_H

/*
 * Physical buttons, as two logical keys the OS understands:
 *
 *   BOOT  - click = back, long press = home
 *   PWR   - click = screen off, long press = power menu
 *
 * Each board maps its real buttons onto them (AMOLED: BOOT GPIO + PMU key;
 * Watcher: the knob button - click, hold, hold longer, double click).
 * Presses also wake the chip from light sleep.
 *
 * The callback runs in the esp_timer task: keep it short (post to a queue).
 */

#include "esp_err.h"

typedef enum
{
    BUTTON_BOOT,
    BUTTON_PWR,
} button_id_t;

typedef enum
{
    BUTTON_CLICK,
    BUTTON_LONG_PRESS,   /* fired once after ~700 ms, while still held */
    BUTTON_DOUBLE_CLICK, /* two quick clicks (boards with a knob: switches focus navigation) */
    BUTTON_RECOVERY,     /* BOOT held for five seconds; persistent headless boot toggle */
} button_event_t;

typedef void (*button_cb_t)(button_id_t button, button_event_t event);

esp_err_t button_init(button_cb_t callback);

/* Read the PWR key latched by the PMU and report it (call periodically). */
void button_poll_pmu(void);

#endif
