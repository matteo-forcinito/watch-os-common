#ifndef RADIO_LIST_H
#define RADIO_LIST_H

/*
 * A list of web radio stations, read from and written to a text file
 * (/sdcard/config/radios.txt, see main/defaults/radios.txt):
 *
 *     # comment
 *     Radio Swiss Jazz | jazz | http://stream.srg-ssr.ch/m/rsj/mp3_128
 *
 * Plain C + stdio: tested on the PC (test/host).
 */

#include <stdbool.h>
#include <stdio.h>

#define RADIO_NAME_MAX  40
#define RADIO_GENRE_MAX 16
#define RADIO_URL_MAX   200
#define RADIO_LIST_MAX  100

typedef struct
{
    char name[RADIO_NAME_MAX];
    char genre[RADIO_GENRE_MAX];
    char url[RADIO_URL_MAX];
} radio_station_t;

typedef struct
{
    int count;
    radio_station_t *stations;   /* RADIO_LIST_MAX entries, on the heap */
} radio_list_t;

/* Allocate an empty list. Returns false if out of memory. */
bool radio_list_init(radio_list_t *list);

void radio_list_free(radio_list_t *list);

/* "name | genre | url" -> station. False for comments, blank or bad lines. */
bool radio_list_parse_line(const char *line, radio_station_t *station);

/* Append the stations found in `text` (whole file in memory). Returns how many. */
int radio_list_parse_text(radio_list_t *list, const char *text);

/* Same, from an open file. */
int radio_list_read(radio_list_t *list, FILE *file);

/* Write the list (with a short explanation at the top). */
void radio_list_write(const radio_list_t *list, FILE *file);

/* Index of the station called `name` (case ignored), or -1. */
int radio_list_find(const radio_list_t *list, const char *name);

/* Add a station (name, url required; '|' is replaced). False if full or invalid. */
bool radio_list_add(radio_list_t *list, const char *name, const char *genre, const char *url);

bool radio_list_remove(radio_list_t *list, int index);

#endif
