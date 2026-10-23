/* A complete first game: copy this file, change the title and the rules.
 * There are no timers, allocations, LVGL calls or hardware details here.
 * See docs/guida/14-minigiochi.md for the Italian walkthrough. */
#include "../minigame_canvas.h"
#include <stdio.h>

#define TAP_DURATION_MS 20000
#define TAP_HIT_SCORE 5
#define TAP_HIT_RADIUS 130
#define TAP_WIN_HITS 10

typedef struct
{
    uint32_t random;
    int elapsed, hits, target_x, target_y, lane;
    bool redraw;
} tap_state_t;

static void new_target(tap_state_t *s)
{
    s->target_x = 250 + (int)(mg_random(&s->random) % 3) * 250;
    s->target_y = 250 + (int)(mg_random(&s->random) % 3) * 200;
    s->redraw = true;
}
static void start(void *state, uint32_t seed)
{
    tap_state_t *s = state;
    s->random = seed;
    s->lane = 1;
    new_target(s);
}
static void step(void *state, int milliseconds)
{
    tap_state_t *s = state;
    s->elapsed += milliseconds;
}
static void input(void *state, const mg_input_t *event)
{
    tap_state_t *s = state;
    bool hit = false;
    if (event->kind == MG_PRESS)
    {
        int dx = event->x - s->target_x, dy = event->y - s->target_y;
        hit = dx * dx + dy * dy <= TAP_HIT_RADIUS * TAP_HIT_RADIUS;
    }
    else if (event->kind == MG_TURN)
    {
        s->lane += event->direction;
        if (s->lane < 0) s->lane = 0;
        if (s->lane > 2) s->lane = 2;
        s->redraw = true;
    }
    else if (event->kind == MG_CLICK)
        hit = 250 + s->lane * 250 == s->target_x;
    if (hit)
    {
        s->hits++;
        new_target(s);
    }
}
static void draw(void *state, mg_canvas_t *canvas)
{
    tap_state_t *s = state;
    if (!s->redraw) return;
    s->redraw = false;
    mg_clear(canvas, 0x17334D);
    mg_circle(canvas, s->target_x, s->target_y, 110, 0x3A6780);
    mg_star(canvas, s->target_x, s->target_y, 70, 0xFFD45C);
    mg_pet(canvas, 250 + s->lane * 250, 950);
}
static void hud(const void *state, char text[MG_HUD_BYTES])
{
    const tap_state_t *s = state;
    int seconds = (TAP_DURATION_MS - s->elapsed + 999) / 1000;
    snprintf(text, MG_HUD_BYTES, "Stelle %d   %ds", s->hits, seconds > 0 ? seconds : 0);
}
static bool result(const void *state, mg_result_t *out)
{
    const tap_state_t *s = state;
    out->score = s->hits * TAP_HIT_SCORE;
    out->won = s->hits >= TAP_WIN_HITS;
    return s->elapsed >= TAP_DURATION_MS;
}
const mg_game_t mg_tap_star = {
    .id = "tap_star", .title = "Tocca la stella", .instructions = "Tocca la stella. Knob: gira e clicca.",
    .lose_text = "Tempo scaduto!", .sky = 0x17334D, .ground = 0x17334D,
    .state_size = sizeof(tap_state_t), .start = start, .step = step,
    .input = input, .draw = draw, .hud = hud, .result = result};
