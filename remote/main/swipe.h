#ifndef SWIPE_H
#define SWIPE_H

/*
 * Swipe typing decoder: turns the path of a finger slid over the letters
 * into the most likely words of a dictionary.
 *
 *   1. candidates: words starting on a key near where the finger went down
 *      and ending on a key near where it lifted;
 *   2. each candidate's ideal path (through its letters' key centers) is
 *      compared with the finger path by DTW (dynamic time warping), which
 *      forgives speed changes and cut corners but needs EVERY part of the
 *      gesture to be explained by the word;
 *   3. score = DTW distance (in key widths) + a small bonus for common words.
 *
 * Plain C (no ESP-IDF, no LVGL), with 20k-word dictionaries.
 * Suggestion accuracy depends on the display geometry and the gesture.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWIPE_LETTERS 26
#define SWIPE_MAX_POINTS 256
#define SWIPE_MAX_WORD 24 /* bytes, UTF-8 */

typedef struct
{
    float x;
    float y;
} swipe_point_t;

/* Where each letter key is (center), and the key size, in pixels. */
typedef struct
{
    swipe_point_t center[SWIPE_LETTERS]; /* index 0 = 'a' */
    float key_w;
    float key_h;
} swipe_layout_t;

typedef struct swipe_dict swipe_dict_t;

/*
 * Build a dictionary from text: one word per line, most frequent first
 * (lowercase; Italian accents à è é ì ò ù allowed). Memory in PSRAM.
 */
swipe_dict_t *swipe_dict_create(const char *text, size_t length);
void swipe_dict_free(swipe_dict_t *dict);
int swipe_dict_size(const swipe_dict_t *dict);

typedef struct
{
    const char *word; /* points into the dictionary */
    float score;      /* lower = better */
} swipe_candidate_t;

/* Decode a finger path. Returns how many candidates (best first, <= max). */
int swipe_decode(const swipe_dict_t *dict, const swipe_layout_t *layout, const swipe_point_t *path, int count,
                 swipe_candidate_t *out, int max);

#endif
