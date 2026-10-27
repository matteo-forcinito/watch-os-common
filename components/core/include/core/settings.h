#ifndef CORE_SETTINGS_H
#define CORE_SETTINGS_H

/*
 * User settings, kept in NVS (internal flash) so they work without SD card.
 *
 *     settings_t s = *settings_get();   // copy
 *     s.brightness = 200;
 *     settings_save(&s);                // applied now, written to flash ~2 s later
 *
 * The write is delayed and merged: dragging a slider does not wear the flash.
 *
 * ADDING A FIELD: append it at the END of settings_t and give it a default
 * in settings.c. Old saved blobs are shorter: the new field keeps its default.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct
{
    /* display */
    uint8_t brightness;          /* 10..255 */
    uint8_t screen_timeout_s;    /* 5..120 */
    bool always_on;              /* dim clock instead of black screen */
    bool tap_to_wake;
    bool wake_on_notification;

    /* sound */
    uint8_t volume;              /* 0..100 */
    bool key_sounds;

    /* time */
    bool time_24h;
    char timezone[48];           /* POSIX TZ, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" */
    bool auto_time;              /* NTP when online, phone time over BLE */

    /* connectivity */
    bool ble_enabled;
    char device_name[24];

    /* weather */
    char weather_city[40];
    float weather_lat;
    float weather_lon;

    /* radio */
    int16_t radio_station;       /* last station index, -1 none */

    /* look */
    uint8_t watchface;           /* 0 = digital, 1 = analog, 2 = minimal */
    uint32_t accent_color;       /* 0xRRGGBB */

    /* firmware update: JSON manifest {"version": "1.2.0", "url": "https://.../x.bin"}; "" = built-in default */
    char update_url[160];

    /* knob boards: true = the knob moves a focus highlight, false = it scrolls */
    bool knob_focus;

    /* online updates: 0 = stable releases, 1 = dev builds too (fw_channel_t) */
    uint8_t update_channel;
    /* Optional perimeter indicators on every display shape. */
    bool border_progress;

    /* launcher: app cards per page (4, 6 or 9) */
    uint8_t launcher_cards;
    /* main menu favourites: app ids ("alarms") or external apps ("ext:Pieno"), "" = free */
    char favorites[6][24];
    uint32_t remote_preferences_version; /* starts after the old persisted struct */
    bool remote_ui_enabled;      /* opt-in fallback mirroring; default off */
    uint32_t ecos_preferences_version;
    uint8_t ecos_mode; /* 0 automatic, 1 background, 2 off outside recovery */
    uint32_t home_preferences_version;
    uint8_t homepage; /* 0 watch face, 1 Casa, 2 app list */
    uint32_t energy_preferences_version;
    bool energy_saver; /* cap display power; preserve user brightness/timeout */
    uint32_t orientation_preferences_version;
    uint8_t screen_rotation; /* clockwise quarter turns; rectangular panels only */
} settings_t;

esp_err_t settings_init(void);

/* Current settings (read only). */
const settings_t *settings_get(void);

/*
 * The accent as a NEON NOIR colour (0xRRGGBB). Values saved by older
 * versions (iOS-like palette) are mapped onto the nearest neon when read;
 * NVS is shared with external apps and is never rewritten for this.
 */
uint32_t settings_accent_hex(void);

/* Replace the settings: applied at once, saved to flash shortly after. */
void settings_save(const settings_t *settings);

/* Write any pending change to flash now (before restart / power off). */
void settings_flush(void);

void settings_reset(void);

/*
 * Called after every settings_save(), in the caller's task: keep it short
 * (compare and sys_post() the real work). Up to 6 listeners.
 */
typedef void (*settings_listener_t)(const settings_t *settings);
void settings_add_listener(settings_listener_t listener);

#endif
