/* Pet game adapters: rules stay in pet_game.c; this file only translates
 * common inputs and draws the two scenes. No LVGL, timers or allocations. */
#include "pet_game_catalog.h"
#include "../minigames/minigame_canvas.h"
#include <stdio.h>
#include <string.h>

typedef struct
{
    pet_game_t model;
    int terrain_y[512], previous_y[512];
    int previous_foot, previous_front;
    bool terrain_ready;
    mg_rect_t previous_items[PET_GAME_ITEMS + 1];
} pet_view_state_t;

static int field_y(const mg_canvas_t *c, int y)
{
    return c->screen_height / 5 + y * (c->screen_height * 58 / 100) / 1000;
}
static void ski_start(void *s, uint32_t seed) { pet_game_start(&((pet_view_state_t *)s)->model, PET_GAME_SKI, seed); }
static void stars_start(void *s, uint32_t seed) { pet_game_start(&((pet_view_state_t *)s)->model, PET_GAME_STARS, seed); }
static void game_step(void *s, int ms) { pet_game_step(&((pet_view_state_t *)s)->model, ms); }
static void game_input(void *s, const mg_input_t *input)
{
    pet_game_t *g = &((pet_view_state_t *)s)->model;
    if (input->kind == MG_PRESS)
    {
        if (g->kind == PET_GAME_SKI) pet_game_hold(g, true);
        else pet_game_input(g, input->x < 500 ? -1 : 1);
    }
    else if (input->kind == MG_RELEASE) pet_game_hold(g, false);
    else if (input->kind == MG_TURN) pet_game_input(g, input->direction);
    else if (input->kind == MG_CLICK)
    {
        if (g->kind == PET_GAME_SKI) pet_game_hold(g, !g->held);
        else pet_game_input(g, 1);
    }
}
static bool game_result(const void *s, mg_result_t *r)
{
    const pet_game_t *g = &((const pet_view_state_t *)s)->model;
    r->score = pet_game_score(g);
    r->won = g->kind == PET_GAME_SKI ? g->won : g->lives > 0;
    return g->done;
}
static void game_hud(const void *s, char text[MG_HUD_BYTES])
{
    const pet_game_t *g = &((const pet_view_state_t *)s)->model;
    if (g->kind == PET_GAME_SKI)
        snprintf(text, MG_HUD_BYTES, "Slancio %d  Valanga %dm  %ds", g->speed, g->gap / 160,
                 (PET_GAME_DURATION_MS - g->elapsed + 999) / 1000);
    else
        snprintf(text, MG_HUD_BYTES, "Punti %d   Vite %d   %ds", pet_game_score(g), g->lives,
                 (PET_GAME_DURATION_MS - g->elapsed + 999) / 1000);
}
static const char *ski_feedback(const void *s)
{
    const pet_game_t *g = &((const pet_view_state_t *)s)->model;
    return g->landing > 0 ? (g->combo ? "Atterraggio perfetto!" : "In salita: perdi slancio!") :
           !g->grounded ? "In volo!" : pet_game_slope(g, g->world / 16) > 0 ? "Tieni premuto in discesa" : "Rilascia sulla cresta";
}
static void ski_draw(void *state, mg_canvas_t *c)
{
    pet_view_state_t *v = state;
    pet_game_t *g = &v->model;
    int W = c->screen_width, H = c->screen_height;
    int top = H * 28 / 100, height = (int)c->height;
    int camera = (g->y < 400 ? g->y - 400 : 0);
    int front = (g->x - g->gap / 16) * W / 1000;
    const uint16_t sky = mg_color(0x367FA8);
    const uint16_t snow = mg_color(0xEFFAFF);
    const uint16_t edge = mg_color(0xB7DFEE);
    /* Two-pixel terrain samples and paired writes keep the PSRAM pass short;
     * the full-size image blits directly without scaling or byte conversion. */
    memcpy(v->previous_y, v->terrain_y, sizeof(v->previous_y));
    for (int x = 0; x < W; x += 2)
    {
        int world = g->world / 16 + x * 1000 / W - g->x;
        v->terrain_y[x] = field_y(c, pet_game_hill(g, world) - camera) - top;
    }
    for (int y = 0; y < height; y += 2)
    {
        uint16_t *row = (uint16_t *)(c->pixels + (size_t)y * c->stride);
        int x = 0;
        for (; x + 1 < W; x += 2)
        {
            uint16_t color = x < front ? edge : y < v->terrain_y[x] ? sky :
                             y < v->terrain_y[x] + 2 ? edge : snow;
            *(uint32_t *)(row + x) = (uint32_t)color | ((uint32_t)color << 16);
        }
        if (x < W) row[x] = y < v->terrain_y[x] ? sky : snow;
        if (y + 1 < height)
            memcpy(c->pixels + (size_t)(y + 1) * c->stride, row, (size_t)W * 2);
    }
    int x = g->x * W / 1000, foot = field_y(c, g->y - camera) - top;
    int dirty_count = c->dirty_count;
    mg_pet_pixels(c, x, foot);
    c->dirty_count = dirty_count; /* The swept actor bounds below include skis. */
    int slope = g->grounded ? pet_game_slope(g, g->world / 16) : 0;
    uint16_t ski = mg_color(0xF7B64B);
    for (int dx = -c->actor_size / 2 - 6; dx < c->actor_size / 2 + 6; dx++)
    {
        int y = foot - 2 + slope * dx * 58 / 100000;
        if (x + dx < 0 || x + dx >= W) continue;
        for (int t = 0; t < 3; t++)
            if (y + t >= 0 && y + t < height)
                *(uint16_t *)(c->pixels + (size_t)(y + t) * c->stride + (x + dx) * 2) = ski;
    }
    if (!v->terrain_ready)
    {
        c->full = true;
        v->terrain_ready = true;
    }
    else
    {

        /* Sky and deep snow are unchanged. Refresh only the swept surface
         * band in narrow segments, plus old/new pet and avalanche bounds. */
        for (int segment = 0; segment < 3; segment++)
        {
            int left = (segment * W / 3) & ~1;
            int right = ((segment + 1) * W / 3) & ~1;
            if (segment == 2) right = W;
            int low = height, high = 0;
            bool changed = false;
            for (int px = left; px < right; px += 2)
            {
                int old = v->previous_y[px], now = v->terrain_y[px];
                if (old != now) changed = true;
                if (old < low) low = old;
                if (now < low) low = now;
                if (old > high) high = old;
                if (now > high) high = now;
            }
            low = low > 2 ? low - 2 : 0;
            high = high + 4 < height ? high + 4 : height - 1;
            if (changed && high >= low)
            {
                mg_rect_t dirty = {left, low,
                                   right - 1, high};
                mg_dirty(c, dirty.x1, dirty.y1, dirty.x2, dirty.y2);
            }
        }
        int low = foot < v->previous_foot ? foot : v->previous_foot;
        int high = foot > v->previous_foot ? foot : v->previous_foot;
        mg_rect_t pet = {x - c->actor_size / 2 - 8,
                         low - c->actor_size - 2,
                         x + c->actor_size / 2 + 8,
                         high + c->actor_size / 2 + 4};
        mg_dirty(c, pet.x1, pet.y1, pet.x2, pet.y2);
        int previous = v->previous_front > 0 ? v->previous_front : 0;
        int current = front > 0 ? front : 0;
        if (current != previous)
        {
            mg_rect_t avalanche = {(current < previous ? current : previous), 0,
                                   (current > previous ? current : previous), height - 1};
            mg_dirty(c, avalanche.x1, avalanche.y1, avalanche.x2, avalanche.y2);
        }
    }
    v->previous_foot = foot;
    v->previous_front = front;
}


static void stars_draw(void *state, mg_canvas_t *c)
{
    pet_view_state_t *v = state;
    pet_game_t *g = &v->model;
    int W = c->screen_width, size = W >= 400 ? 24 : 16;
    if (!v->terrain_ready)
    {
        mg_clear(c, 0x11152F);
        v->terrain_ready = true;
    }
    else
        for (int i = 0; i <= PET_GAME_ITEMS; i++)
        {
            mg_rect_t *r = &v->previous_items[i];
            mg_box_pixels(c, r->x1, r->y1, r->x2 - r->x1 + 1, r->y2 - r->y1 + 1, 0x11152F);
        }
    for (int i = 0; i < PET_GAME_ITEMS; i++)
    {
        const pet_game_item_t *p = &g->items[i];
        mg_rect_t *r = &v->previous_items[i];
        *r = (mg_rect_t){0, 0, -1, -1};
        if (!p->used) continue;
        int x = p->x * W / 1000, y = field_y(c, p->y) - c->top;
        *r = (mg_rect_t){x - size, y - size, x + size, y + size};
        if (p->star) mg_star(c, p->x, y * 1000 / c->height, 45, 0xFFE8AA);
        else mg_box_pixels(c, x - size / 2, y - size, size, size, 0x526277);
    }
    int x = g->x * W / 1000, foot = field_y(c, g->y) - c->top;
    if (g->invincible <= 0 || g->elapsed / 132 % 2) mg_pet_pixels(c, x, foot);
    v->previous_items[PET_GAME_ITEMS] = (mg_rect_t){x - c->actor_size / 2, foot - c->actor_size,
                                                                 x + c->actor_size / 2, foot};
}

static const mg_game_t s_ski = {
    .id = "ski", .actor_walk = true, .title = "Fuga sulla neve", .instructions = "Tieni in discesa, lascia sulla cresta",
    .lose_text = "Valanga!", .win_text = "Salvo!", .sky = 0x367FA8, .ground = 0xEFFAFF,
    .state_size = sizeof(pet_view_state_t), .start = ski_start, .step = game_step,
    .input = game_input, .draw = ski_draw, .hud = game_hud, .result = game_result, .feedback = ski_feedback};
static const mg_game_t s_stars = {
    .id = "stars", .title = "Pioggia di stelle", .instructions = "Tocca a sinistra/destra o gira il Knob",
    .lose_text = "Partita finita!", .sky = 0x11152F, .ground = 0x242545,
    .state_size = sizeof(pet_view_state_t), .start = stars_start, .step = game_step,
    .input = game_input, .draw = stars_draw, .hud = game_hud, .result = game_result};
extern const mg_game_t mg_tap_star;
extern const mg_game_t mg_quantum;
/* Generated game declarations. */
static const mg_game_t *const s_games[] = {
    &s_ski, &s_stars, &mg_tap_star,
    &mg_quantum,
    /* Generated game entries. */
};
size_t pet_game_count(void) { return sizeof(s_games) / sizeof(s_games[0]); }
const mg_game_t *pet_game_at(size_t index) { return index < pet_game_count() ? s_games[index] : NULL; }
const mg_game_t *pet_game_find(const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < pet_game_count(); i++)
        if (!strcmp(id, s_games[i]->id)) return s_games[i];
    return NULL;
}
