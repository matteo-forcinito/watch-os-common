/*
 * Deck - a programmable macro pad for the PC (USB or Bluetooth keyboard).
 *
 *   action.c    pure C: "ctrl+shift+m; wait:200; type:..." -> steps
 *   profiles.c  built-in profiles (meetings, Windows, code, browser, media)
 *               + custom profiles from <storage>/config/deck.json
 *   run.c       worker: executes actions and the dial in order (never in the UI)
 *   web.c       configuration mode: the editor page on the PC (Wi-Fi only;
 *               the app restarts to switch between HID and configuration)
 *   ui.c        keycaps grid, profile swipe, dial (knob or touch strip)
 */
#ifndef DECK_H
#define DECK_H

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DECK_KEYS 12
#define DECK_CUSTOM 6
#define DECK_PROFILES (6 + DECK_CUSTOM)

typedef enum
{
    DECK_DIAL_VOLUME,
    DECK_DIAL_SCROLL,
    DECK_DIAL_ZOOM,
    DECK_DIAL_TABS,
    DECK_DIAL_UNDO,
    DECK_DIAL_ARROWS,
    DECK_DIAL_ARROWS_VERTICAL,
    DECK_DIAL_COUNT,
} deck_dial_t;

typedef struct
{
    char label[24];
    char glyph[16]; /* symbol name ("mic", "play"...) or short text ("F5") */
    uint32_t color;
    char action[160];
} deck_key_t;

typedef struct
{
    char name[24];
    uint8_t dial;
    bool custom;
    uint8_t count;
    deck_key_t keys[DECK_KEYS];
} deck_profile_t;

/* Profiles */
esp_err_t deck_profiles_load(void);
int deck_profile_count(void);
const deck_profile_t *deck_profile(int index);
/* Validate and store custom profiles from editor JSON; error explains a refusal. */
esp_err_t deck_profiles_store(const char *json, char *error, size_t error_size);
/* Every profile as JSON for the editor (heap, caller frees). */
char *deck_profiles_json(void);
const char *deck_dial_name(deck_dial_t dial);
const char *deck_dial_id(deck_dial_t dial);
/* Glyph name -> LVGL symbol text, or the text itself. */
const char *deck_glyph(const char *name);

/* Settings (NVS "deck") */
typedef struct
{
    uint8_t transport; /* hid_link_transport_t */
    uint8_t profile;
    uint8_t config_mode; /* 1: next start is the configuration page */
} deck_settings_t;
const deck_settings_t *deck_settings(void);
void deck_settings_save(const deck_settings_t *settings);

/* Run */
void deck_run_start(void);
bool deck_run(const char *action);       /* queued; false if the queue is full */
void deck_dial(deck_dial_t dial, int delta);
bool deck_connected(void);

/* Configuration mode */
esp_err_t deck_web_start(void);
const char *deck_web_code(void);
const char *deck_web_address(void);
bool deck_web_saved(void); /* true once the editor saved (UI offers to go back) */

#endif
