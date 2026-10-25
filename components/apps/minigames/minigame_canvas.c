#include "minigame_canvas.h"
#include <string.h>
#include <limits.h>

uint16_t mg_color(uint32_t rgb)
{
    uint16_t v = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
    return (uint16_t)((v >> 8) | (v << 8));
}
static int point(int value, int size)
{
    /* Bound novice input before multiplying, including INT_MIN/INT_MAX. */
    if (value < -4000) value = -4000;
    if (value > 4000) value = 4000;
    return value * size / 1000;
}
void mg_canvas_begin(mg_canvas_t *c)
{
    c->full = false;
    c->dirty_count = 0;
}
void mg_dirty(mg_canvas_t *c, int x1, int y1, int x2, int y2)
{
    if (c->full || x2 < 0 || y2 < 0 || x1 >= c->width || y1 >= c->height || x2 < x1 || y2 < y1) return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= c->width) x2 = c->width - 1;
    if (y2 >= c->height) y2 = c->height - 1;
    if (c->dirty_count == MG_DIRTY_RECTS)
    {
        c->full = true;
        c->dirty_count = 0;
        return;
    }
    c->dirty[c->dirty_count++] = (mg_rect_t){x1, y1, x2, y2};
}
static void rect_pixels(mg_canvas_t *c, int x1, int y1, int x2, int y2, uint16_t color)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= c->width) x2 = c->width - 1;
    if (y2 >= c->height) y2 = c->height - 1;
    if (x2 < x1 || y2 < y1) return;
    for (int y = y1; y <= y2; y++)
    {
        uint16_t *row = (uint16_t *)(c->pixels + (size_t)y * c->stride);
        for (int x = x1; x <= x2; x++) row[x] = color;
    }
    mg_dirty(c, x1, y1, x2, y2);
}
void mg_box_pixels(mg_canvas_t *c, int x, int y, int width, int height, uint32_t rgb)
{
    if (width <= 0 || height <= 0 || x < -10000 || y < -10000 || x > 10000 || y > 10000 || width > 10000 || height > 10000) return;
    rect_pixels(c, x, y, x + width - 1, y + height - 1, mg_color(rgb));
}
void mg_fill_pixels(mg_canvas_t *c, uint32_t rgb)
{
    uint16_t color = mg_color(rgb);
    uint32_t pair = (uint32_t)color | ((uint32_t)color << 16);
    for (int y = 0; y < c->height; y++)
    {
        uint16_t *row = (uint16_t *)(c->pixels + (size_t)y * c->stride);
        int x = 0;
        for (; x + 1 < c->width; x += 2) memcpy(row + x, &pair, sizeof(pair));
        if (x < c->width) row[x] = color;
    }
}
void mg_clear(mg_canvas_t *c, uint32_t rgb)
{
    mg_fill_pixels(c, rgb);
    c->full = true;
}
void mg_rect(mg_canvas_t *c, int x, int y, int w, int h, uint32_t rgb)
{
    if (w <= 0 || h <= 0) return;
    int px = point(x, c->width), py = point(y, c->height);
    rect_pixels(c, px, py, px + point(w, c->width) - 1, py + point(h, c->height) - 1, mg_color(rgb));
}
void mg_circle(mg_canvas_t *c, int x, int y, int radius, uint32_t rgb)
{
    if (radius <= 0) return;
    int px = point(x, c->width), py = point(y, c->height);
    int r = point(radius, c->width < c->height ? c->width : c->height);
    if (r > c->width + c->height) r = c->width + c->height;
    uint16_t color = mg_color(rgb);
    int left = px - r, right = px + r, top = py - r, bottom = py + r;
    if (top < 0) top = 0;
    if (bottom >= c->height) bottom = c->height - 1;
    if (left < 0) left = 0;
    if (right >= c->width) right = c->width - 1;
    for (int yy = top; yy <= bottom; yy++)
    {
        uint16_t *row = (uint16_t *)(c->pixels + (size_t)yy * c->stride);
        for (int xx = left; xx <= right; xx++)
            if ((int64_t)(xx - px) * (xx - px) + (int64_t)(yy - py) * (yy - py) <= (int64_t)r * r) row[xx] = color;
    }
    mg_dirty(c, left, top, right, bottom);
}
void mg_star(mg_canvas_t *c, int x, int y, int r, uint32_t rgb)
{
    if (r <= 0) return;
    if (r > 4000) r = 4000;
    if (x < -4000) x = -4000;
    if (x > 4000) x = 4000;
    if (y < -4000) y = -4000;
    if (y > 4000) y = 4000;
    /* A four-point pixel star, using clipped rectangles. */
    int arm = r > 10 ? r / 3 : 3;
    mg_rect(c, x - arm, y - r, arm * 2, r * 2, rgb);
    mg_rect(c, x - r, y - arm, r * 2, arm * 2, rgb);
}
void mg_pet_pixels(mg_canvas_t *c, int x, int foot)
{
    if (!c->actor_grid || !c->actor_palette || c->actor_size <= 0) return;
    int scale = c->actor_size / 32;
    if (!scale) return;
    for (int py = 0; py < c->actor_size; py++)
    {
        int y = foot - c->actor_size + py;
        if (y < 0 || y >= c->height) continue;
        uint16_t *row = (uint16_t *)(c->pixels + (size_t)y * c->stride);
        for (int px = 0; px < c->actor_size; px++)
        {
            int dx = x - c->actor_size / 2 + px;
            uint8_t index = c->actor_grid[(py / scale) * 32 + px / scale];
            int colors = c->actor_colors > 0 ? c->actor_colors : 12;
            if (dx >= 0 && dx < c->width && index > 0 && index < colors) row[dx] = c->actor_palette[index];
        }
    }
    mg_dirty(c, x - c->actor_size / 2, foot - c->actor_size, x + c->actor_size / 2, foot);
}
void mg_pet(mg_canvas_t *c, int x, int foot)
{
    mg_pet_pixels(c, point(x, c->width), point(foot, c->height));
}
