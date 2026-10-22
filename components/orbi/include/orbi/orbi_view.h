#ifndef ORBI_VIEW_H
#define ORBI_VIEW_H

/*
 * An Orbi creature on screen, drawn from its genome exactly as the companion
 * draws it (docs/ORBI-GENOME.md, "Drawing"). Small LVGL objects that move
 * (no transforms): it floats, blinks, looks around, reacts to a poke.
 * LVGL task only (or with lv_port_lock()).
 */

#include "lvgl.h"
#include "orbi/orbi_genome.h"

typedef enum { ORBI_HAPPY, ORBI_CALM, ORBI_SAD, ORBI_SLEEPY, ORBI_SLEEP, ORBI_LOVE } orbi_mood_t;

typedef struct orbi_view orbi_view_t;

/* width = body width in pixels; the view is about 1.5 x width wide. */
orbi_view_t *orbi_view_create(lv_obj_t *parent, const orbi_genome_t *genome, int32_t width);
lv_obj_t *orbi_view_root(orbi_view_t *view);   /* align it like any object */
void orbi_view_mood(orbi_view_t *view, orbi_mood_t mood);
/* Look towards x, y in -100..100 (e.g. the finger or the tilt); look_free() wanders again. */
void orbi_view_look(orbi_view_t *view, int x, int y);
void orbi_view_look_free(orbi_view_t *view);
void orbi_view_poke(orbi_view_t *view);
/* The view is freed when its root object is deleted (or with this). */
void orbi_view_delete(orbi_view_t *view);

#endif
