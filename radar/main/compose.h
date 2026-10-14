/*
 * Radar picture maths, pure C (no ESP-IDF, no libc: host/emulator tested).
 *
 * Coordinates: Web Mercator "global pixels" at the base zoom. The screen
 * pixel (sx, sy) shows global pixel (ox + sx, oy + sy). A tile of zoom z sits
 * at (tx * 256, ty * 256) in its own zoom; when the source zoom is lower than
 * the base zoom by k, one source pixel covers 2^k x 2^k screen pixels (scale).
 */
#ifndef RADAR_COMPOSE_H
#define RADAR_COMPOSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t rgba[256];   /* index 0 = transparent */
    uint16_t rgb565[256];
    uint8_t alpha[256];
    int count;
    uint16_t slots[1024]; /* hash: rgba -> index + 1 (0 = empty) */
} radar_palette_t;

void radar_palette_init(radar_palette_t *p);
/* Index of a colour (adds it if new; 0 for transparent or when full). */
uint8_t radar_palette_index(radar_palette_t *p, uint32_t rgba);

/* Base map: an OSM tile (RGBA) darkened into the RGB565 view. */
void radar_blit_base(uint16_t *view, int w, int h, int32_t ox, int32_t oy, const uint8_t *tile_rgba, int32_t tx,
                     int32_t ty);
/* Radar: a tile (RGBA) of a zoom `shift` levels lower into the indexed view. */
void radar_blit_radar(uint8_t *view, int w, int h, int32_t ox, int32_t oy, int shift, const uint8_t *tile_rgba,
                      int32_t tx, int32_t ty, radar_palette_t *p);

/* out = base with the frame blended on top. */
void radar_compose_frame(const uint16_t *base, const uint8_t *frame, const radar_palette_t *p, uint16_t *out, int n);

/* Nearest pixel with rain (alpha >= min_alpha) from (cx, cy): distance in
 * pixels and direction (degrees clockwise from north). False if none. */
bool radar_nearest_rain(const uint8_t *frame, const radar_palette_t *p, int w, int h, int cx, int cy, int min_alpha,
                        float *distance_px, float *bearing_deg);
/* Mean alpha (0..1) of the frame within radius pixels of (cx, cy). */
float radar_intensity_at(const uint8_t *frame, const radar_palette_t *p, int w, int h, int cx, int cy, int radius);

/* Atan2 in degrees clockwise from north (screen: y grows down). Pure C. */
float radar_bearing(float dx, float dy);
float radar_sqrt(float x);

#endif
