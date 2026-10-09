#ifndef SERVICES_ALARM_H
#define SERVICES_ALARM_H

/*
 * Alarms: list, schedule, snooze.
 *
 * Kept in NVS (they work without SD card). At first start the alarms of the
 * Arduino launcher (/sdcard/alarms.json) are imported.
 *
 * A one-shot esp_timer is armed for the next alarm (at most one minute
 * ahead, so a time-zone or clock change is picked up quickly). It keeps
 * running in light sleep; when it fires, the on_ring callback runs (the app
 * layer wakes the screen and opens the ring screen).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define ALARM_MAX        16
#define ALARM_LABEL_MAX  32

/* Day bits: bit0 = Monday ... bit6 = Sunday. 0 = rings once. */
#define ALARM_DAYS_ONCE      0x00
#define ALARM_DAYS_WEEKDAYS  0x1F
#define ALARM_DAYS_EVERYDAY  0x7F

typedef struct
{
    uint32_t id;            /* 0 = new */
    uint8_t hour;
    uint8_t minute;
    uint8_t days;
    bool enabled;
    int16_t radio;          /* station index to wake up with, -1 = melody */
    char label[ALARM_LABEL_MAX];
} alarm_t;

typedef void (*alarm_ring_cb_t)(const alarm_t *alarm);

void alarm_service_init(alarm_ring_cb_t on_ring);

int alarm_list(alarm_t *out, int max);
bool alarm_get(uint32_t id, alarm_t *out);

/* Add (id 0) or update. Returns the id in *alarm. */
esp_err_t alarm_save(alarm_t *alarm);
esp_err_t alarm_delete(uint32_t id);
esp_err_t alarm_set_enabled(uint32_t id, bool enabled);

/* Next alarm that will ring; false if none. */
bool alarm_next(alarm_t *out, time_t *when);

/* While ringing: ring again in `minutes`, or stop. */
void alarm_snooze(uint32_t id, int minutes);
void alarm_dismiss(uint32_t id);

/* "Lun Mar Mer", "Ogni giorno", "Una volta"... */
void alarm_days_text(uint8_t days, char *out, size_t size);

#endif
