/*
 * Ombra - what is transmitting around you over Bluetooth LE.
 *
 *   scanner.c  NimBLE observer -> device table (PSRAM), smoothing, distance,
 *              "is it following me?" detection, devices marked as mine (NVS)
 *   classify.c pure C: advertisement -> kind, tracker model, name, company
 *   sound.c    speaker: Geiger-counter clicks while finding, alert beeps
 *   ui.c       pages (field, nearby, detail, find, shield, composition)
 *
 * Threads: the NimBLE host task updates the table under a mutex; the UI copies
 * snapshots from the lvgl task (never holds the mutex while drawing).
 */
#ifndef OMBRA_H
#define OMBRA_H

#include "classify.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#define OMBRA_DEVICES 128
#define OMBRA_HISTORY 60 /* one RSSI sample per second */

typedef enum
{
    OMBRA_ADDR_PUBLIC,
    OMBRA_ADDR_STATIC,  /* random static: fixed until power cycle */
    OMBRA_ADDR_PRIVATE, /* resolvable private: changes every few minutes */
    OMBRA_ADDR_ANON,    /* non-resolvable: anonymous */
} ombra_addr_t;

typedef struct
{
    uint32_t id; /* stable while the device stays in the table */
    uint8_t mac[6];
    uint8_t addr_type; /* ombra_addr_t */
    ombra_adv_t adv;
    int8_t rssi;  /* smoothed */
    int8_t last;  /* last frame */
    int8_t best;
    int8_t reference; /* RSSI expected at 1 m */
    uint32_t distance_cm;
    uint32_t first_ms, last_ms, follow_ms; /* follow_ms: how long it has stayed with you */
    uint32_t frames;
    int8_t history[OMBRA_HISTORY]; /* oldest first, -127 = no sample */
    bool mine;
    bool suspect; /* separated tracker, not mine, with you longer than the threshold */
} ombra_device_t;

typedef struct
{
    int total, trackers, suspects, mine;
    int kinds[OMBRA_KIND_COUNT];
    bool scanning;
    bool shield;
    int threshold_min;
    uint32_t revision; /* changes whenever the table changes */
    uint32_t alert_id; /* last device that raised an alert (0 = none) */
    uint32_t uptime_ms;
} ombra_status_t;

esp_err_t ombra_start(void);
void ombra_status(ombra_status_t *out);

/* Copy devices sorted nearest first; filter = -1 all, else an ombra_kind_t. */
int ombra_snapshot(ombra_device_t *out, int max, int filter);
bool ombra_get(uint32_t id, ombra_device_t *out);

void ombra_set_mine(uint32_t id, bool mine);
void ombra_set_shield(bool on);
void ombra_set_threshold(int minutes);

/* Name for lists: the advertised name, else the model, else company + kind. */
void ombra_display_name(const ombra_device_t *d, char *out, size_t size);
const char *ombra_addr_text(ombra_addr_t type);

/* ui.c (lvgl task) */
#include "lvgl.h"
void ombra_ui_create(lv_obj_t *screen);
bool ombra_ui_back(void);

/* sound.c */
void ombra_sound_init(void);
bool ombra_sound_available(void);
void ombra_sound_find(uint32_t id); /* 0 = silent */
void ombra_sound_enable(bool on);
bool ombra_sound_enabled(void);
void ombra_sound_alert(void);

#endif
