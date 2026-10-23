#ifndef MINIGAME_CANVAS_H
#define MINIGAME_CANVAS_H
#include "minigame.h"

/* Public drawing helpers use a 0..1000 play area on every display. Rectangles
 * and sprites are clipped: a falling object may safely leave the screen. */
void mg_clear(mg_canvas_t *c, uint32_t rgb);
void mg_rect(mg_canvas_t *c, int x, int y, int width, int height, uint32_t rgb);
void mg_circle(mg_canvas_t *c, int x, int y, int radius, uint32_t rgb);
void mg_star(mg_canvas_t *c, int x, int y, int radius, uint32_t rgb);
void mg_pet(mg_canvas_t *c, int x, int foot_y);
uint16_t mg_color(uint32_t rgb);

/* Advanced renderers can use native pixels and selective invalidation. Most
 * games should only use the helpers above. Coordinates below are in pixels. */
#define MG_DIRTY_RECTS 24
typedef struct { int x1, y1, x2, y2; } mg_rect_t;
struct mg_canvas
{
    uint8_t *pixels;
    int width, height, stride, screen_width, screen_height, top;
    const uint8_t *actor_grid;
    const uint16_t *actor_palette;
    int actor_size;
    bool full;
    int dirty_count;
    mg_rect_t dirty[MG_DIRTY_RECTS];
};
void mg_canvas_begin(mg_canvas_t *c);
void mg_dirty(mg_canvas_t *c, int x1, int y1, int x2, int y2);
void mg_box_pixels(mg_canvas_t *c, int x, int y, int width, int height, uint32_t rgb);
void mg_fill_pixels(mg_canvas_t *c, uint32_t rgb);
void mg_pet_pixels(mg_canvas_t *c, int x, int foot_y);
#endif
