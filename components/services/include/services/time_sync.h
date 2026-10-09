#ifndef SERVICES_TIME_SYNC_H
#define SERVICES_TIME_SYNC_H

/*
 * Keeps the clock right without wasting battery:
 *
 *   - whenever Wi-Fi is connected anyway (radio, weather...), NTP runs once
 *     if the last sync is older than 6 hours;
 *   - once a day, if the clock was not synced by any means, Wi-Fi is turned
 *     on just for NTP (a few seconds) and off again;
 *   - the phone also sets the time over Bluetooth (services/ble_companion).
 *
 * Every successful sync is written to the RTC.
 */

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

void time_sync_init(void);

/* Sync now (turns Wi-Fi on if needed). Blocking, up to ~20 s. */
esp_err_t time_sync_now(void);

/* Somebody (the phone) set the time: counts as a sync. */
void time_sync_mark(void);

/* When the clock was last synced (0 = never since boot). */
time_t time_sync_last(void);

#endif
