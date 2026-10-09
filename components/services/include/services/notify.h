#ifndef SERVICES_NOTIFY_H
#define SERVICES_NOTIFY_H

/*
 * Notifications received from the phone (BLE companion), newest first.
 * Kept in RAM only (a watch does not need yesterday's messages).
 * Changes bump STATE_NOTIF_VERSION; STATE_NOTIF_COUNT holds the count.
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define NOTIFY_MAX 20

typedef struct
{
    uint32_t id;          /* the phone's id (to remove it later) */
    time_t time;
    char app[24];         /* "WhatsApp", "Gmail"... */
    char title[48];
    char body[200];
} notify_t;

void notify_init(void);

/* Add (or replace, same id). Wakes the screen and chimes if enabled. */
void notify_add(const notify_t *n);
void notify_remove(uint32_t id);
void notify_clear(void);

int notify_count(void);

/* Copy of entry `index` (0 = newest). */
bool notify_get(int index, notify_t *out);

/* Incoming call (from the phone): shown full screen. */
typedef struct
{
    bool active;
    char name[48];
    char number[24];
} notify_call_t;

void notify_set_call(const notify_call_t *call);
bool notify_get_call(notify_call_t *out);

#endif
