#ifndef CORE_STATE_H
#define CORE_STATE_H

/*
 * Shared system state, observable by the UI.
 *
 * Each value is an LVGL "subject": a widget can bind to it and update
 * itself whenever it changes, e.g.
 *
 *     lv_label_bind_text(label, state_subject(STATE_BATTERY), "%d%%");
 *
 * Services (other tasks) call state_set() at any time: it never blocks and
 * never takes the LVGL lock. The new value is applied to the subject in the
 * lvgl task, which then notifies the observers.
 *
 * For data that is not a single number (radio title, weather, notifications)
 * the service keeps the data and bumps a *_VERSION counter here; observers
 * then read the data through the service's thread-safe getters.
 */

#include <stdint.h>

#include "lvgl.h"

typedef enum
{
    STATE_BATTERY,          /* 0..100, -1 unknown */
    STATE_CHARGING,         /* 0/1 */
    STATE_USB,              /* 0/1 */
    STATE_WIFI,             /* state_wifi_t */
    STATE_BLE,              /* state_ble_t */
    STATE_MINUTE,           /* minutes since 1970 (local clock tick, updated each minute) */
    STATE_SCREEN,           /* power_screen_t */
    STATE_RADIO,            /* radio state (services/radio), bumps on change */
    STATE_RADIO_VERSION,    /* station/title changed */
    STATE_NOTIF_VERSION,    /* notification list changed */
    STATE_NOTIF_COUNT,
    STATE_WEATHER_VERSION,
    STATE_ALARM_VERSION,
    STATE_SETTINGS_VERSION,
    STATE_MUSIC_VERSION,    /* phone music info (from the companion) */
    STATE_UPDATE,           /* fw_update_phase_t (services/fw_update.h) */
    STATE_UPDATE_PERCENT,   /* 0..100 while writing a firmware */
    STATE_COUNT
} state_id_t;

typedef enum
{
    STATE_WIFI_OFF,
    STATE_WIFI_CONNECTING,
    STATE_WIFI_CONNECTED,
    STATE_WIFI_AP,          /* our own access point (setup page) */
} state_wifi_t;

typedef enum
{
    STATE_BLE_OFF,
    STATE_BLE_ADVERTISING,
    STATE_BLE_CONNECTED,
} state_ble_t;

/* Called by lv_port_init(). */
void state_init(void);

/* Set from any task (non-blocking). */
void state_set(state_id_t id, int32_t value);

/* Add 1 (for *_VERSION counters). */
void state_bump(state_id_t id);

/* Latest value (may not yet be applied to the subject). */
int32_t state_get(state_id_t id);

/* The LVGL subject (use only in the lvgl task). */
lv_subject_t *state_subject(state_id_t id);

/* lvgl task only: push pending values to the subjects. */
void state_apply_pending(void);

#endif
