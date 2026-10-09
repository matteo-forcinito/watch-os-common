#ifndef SERVICES_RADIO_H
#define SERVICES_RADIO_H

/*
 * The web radio as the user sees it: a station list, favorites, the station
 * playing now. Built on radio_player (the streaming engine).
 *
 * Commands (play, stop, next...) are queued to a small worker task and
 * return at once, so the UI never waits for the network. The UI observes
 * STATE_RADIO (state) and STATE_RADIO_VERSION (station / song title).
 *
 * Stations: /sdcard/config/radios.txt if present, otherwise the built-in list
 * (components/services/radio/radios_default.txt). Favorites are kept in NVS
 * by station name; the default ones are the first six of the built-in list.
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "webradio/radio_player.h"

#define RADIO_FAVORITES_MAX 12

typedef struct
{
    char name[40];
    char genre[16];
    char url[200];
} radio_info_t;

void radio_service_init(void);

int radio_count(void);
bool radio_get(int index, radio_info_t *out);

/* Favorites, in the order the user starred them. Returns how many. */
int radio_favorites(int *indexes, int max);
bool radio_is_favorite(int index);
void radio_set_favorite(int index, bool favorite);

/* Async commands. */
void radio_play(int index);
void radio_stop(void);
void radio_toggle(void);        /* stop, or play the last station */
void radio_next(int direction); /* +1 / -1, within favorites when playing one */

/* Station playing (or starting) now, -1 if none. Last one played if stopped. */
int radio_current(void);
int radio_last(void);
bool radio_is_on(void);         /* playing or starting */

void radio_set_volume(int percent);   /* saved in the settings */

/* Re-read the station list (after the web page changed it). */
void radio_reload(void);

/* Edit the list on the SD card. */
esp_err_t radio_add(const char *name, const char *genre, const char *url);
esp_err_t radio_remove(int index);

/* The list as text (radios.txt format), for the web page. Caller frees. */
char *radio_export_text(void);

#endif
