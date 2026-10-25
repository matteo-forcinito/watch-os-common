#include "minigame.h"
#include "minigame_canvas.h"
#include "pet_game_catalog.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* State changes make fixed-step, pause and one-shot behavior observable. */
typedef struct { int elapsed, presses, releases; bool held; } test_state_t;
static void start(void *p, uint32_t seed) { (void)p; (void)seed; }
static void step(void *p, int ms) { ((test_state_t *)p)->elapsed += ms; }
static void input(void *p, const mg_input_t *e)
{
    test_state_t *s = p;
    assert(e->x >= 0 && e->x <= 1000 && e->y >= 0 && e->y <= 1000);
    assert(e->direction >= -1 && e->direction <= 1);
    if (e->kind == MG_PRESS) s->presses++, s->held = true;
    if (e->kind == MG_RELEASE) s->releases++, s->held = false;
}
static void draw(void *p, mg_canvas_t *c) { (void)p; (void)c; }
static bool result(const void *p, mg_result_t *r)
{
    r->score = 900;
    r->won = true;
    return ((const test_state_t *)p)->elapsed >= 1000;
}
static const mg_game_t game = {.id = "test", .title = "Test", .state_size = sizeof(test_state_t),
                              .start = start, .step = step, .input = input, .draw = draw, .result = result};
extern const mg_game_t mg_demo_balloons;
int main(void)
{
    mg_session_t *example = mg_create(&mg_demo_balloons, 1, 0);
    assert(example);
    mg_destroy(example);
    assert(!mg_create(NULL, 0, 0));
    mg_game_t bad = game;
    bad.state_size = MG_MAX_STATE_BYTES + 1;
    assert(!mg_create(&bad, 0, 0));
    bad = game; bad.draw = NULL;
    assert(!mg_create(&bad, 0, 0));
    mg_session_t *s = mg_create(&game, 0, 0);
    assert(s);
    mg_advance(s, 33, true);
    assert(mg_elapsed(s) == 32);
    mg_input(s, MG_PRESS, INT_MIN, INT_MAX, INT_MIN);
    assert(((test_state_t *)mg_state(s))->held);
    mg_advance(s, 10000, false);
    assert(mg_elapsed(s) == 32 && !((test_state_t *)mg_state(s))->held);
    mg_advance(s, 20000, true);
    assert(mg_elapsed(s) == 32);
    mg_advance(s, 20016, true);
    assert(mg_elapsed(s) == 48);
    mg_advance(s, 1000000, true);
    assert(mg_elapsed(s) <= 304); /* bounded catch-up */
    mg_restart(s, 123, 0);
    for (uint64_t now = 16; now <= 1100; now += 16) mg_advance(s, now, true);
    mg_result_t r;
    assert(mg_done(s) && mg_take_result(s, &r) && r.score == 100 && r.won);
    assert(!mg_take_result(s, &r));
    int elapsed = mg_elapsed(s);
    mg_input(s, MG_PRESS, 1, 1, 1);
    mg_advance(s, 999999, true);
    assert(mg_elapsed(s) == elapsed);
    mg_restart(s, 10, 400);
    assert(!mg_done(s) && !mg_take_result(s, &r) && mg_elapsed(s) == 0);
    assert(((test_state_t *)mg_state(s))->presses == 0);
    mg_destroy(s);

    /* Draw the actual registered games at each board's dimensions with
     * sentinels around the framebuffer; clipped/offscreen objects stay safe. */
    const int dimensions[][2] = {{368,448},{466,466},{360,360},{412,412},{480,480}};
    uint8_t actor[1024]; memset(actor, 2, sizeof(actor));
    uint16_t palette[12] = {0}; palette[2] = mg_color(0xFF0000);
    assert(mg_color(0xFF0000) == 0x00F8 && mg_color(0xFFFFFF) == 0xFFFF);
    for (size_t board = 0; board < sizeof(dimensions)/sizeof(dimensions[0]); board++)
    {
        int w = dimensions[board][0], h = dimensions[board][1] * 45 / 100;
        size_t bytes = (size_t)w * h * 2;
        uint8_t *memory = malloc(bytes + 32); assert(memory);
        memset(memory, 0xA5, bytes + 32);
        mg_canvas_t c = {.pixels = memory + 16, .width = w, .height = h, .stride = w * 2,
                          .screen_width = w, .screen_height = dimensions[board][1],
                          .top = dimensions[board][1] * 28 / 100, .actor_grid = actor,
                          .actor_palette = palette, .actor_size = w >= 400 ? 64 : 32};
        mg_canvas_begin(&c);
        mg_rect(&c, INT_MIN, INT_MAX, INT_MAX, INT_MAX, 0xFFFFFF);
        mg_circle(&c, INT_MIN, INT_MAX, INT_MAX, 0xFFFFFF);
        mg_star(&c, INT_MIN, INT_MAX, INT_MAX, 0xFFFFFF);
        mg_pet(&c, -1000, 2000);
        /* Optional Pokemon palettes include indices above the legacy 12.
         * An explicit count must paint them without touching guard bytes. */
        uint16_t full_palette[256] = {0};
        full_palette[200] = mg_color(0x00FF00);
        memset(actor, 200, sizeof(actor));
        c.actor_palette = full_palette;
        c.actor_colors = 256;
        mg_pet_pixels(&c, c.actor_size / 2, c.actor_size);
        assert(*(uint16_t *)c.pixels == full_palette[200]);
        memset(actor, 2, sizeof(actor));
        c.actor_palette = palette;
        c.actor_colors = 0;
        for (size_t i = 0; i < pet_game_count(); i++)
        {
            const mg_game_t *g = pet_game_at(i);
            assert(g && pet_game_find(g->id) == g);
            s = mg_create(g, 2, 0); assert(s);
            for (uint64_t now = 0; now < 61000 && !mg_done(s); now += 33)
            {
                if (now % 99 == 0) mg_input(s, MG_PRESS, (int)(now % 1000), (int)(now * 3 % 1000), -1);
                else mg_input(s, MG_RELEASE, 0, 0, 0);
                mg_advance(s, now, true);
                mg_canvas_begin(&c);
                g->draw(mg_state(s), &c);
                assert(c.dirty_count <= MG_DIRTY_RECTS);
                for (int n = 0; n < 16; n++) assert(memory[n] == 0xA5 && memory[bytes + 16 + n] == 0xA5);
                char hud[MG_HUD_BYTES] = {0};
                if (g->hud) g->hud(mg_state(s), hud);
                assert(memchr(hud, 0, sizeof(hud)));
            }
            if (g->continuous)
            {
                assert(!mg_done(s) && !mg_take_result(s, &r));
                uint8_t checkpoint[MG_CHECKPOINT_BYTES];
                assert(g->checkpoint_bytes <= sizeof(checkpoint));
                assert(g->save(mg_state(s), checkpoint, g->checkpoint_bytes));
                mg_session_t *resumed = mg_create(g, 55, 0);
                assert(resumed && g->load(mg_state(resumed), checkpoint, g->checkpoint_bytes));
                mg_canvas_begin(&c);
                g->draw(mg_state(resumed), &c);
                assert(!g->load(mg_state(resumed), checkpoint, g->checkpoint_bytes - 1));
                mg_destroy(resumed);
            }
            else assert(mg_done(s) && mg_take_result(s, &r) && r.score >= 0 && r.score <= 100);
            mg_destroy(s);
        }
        free(memory);
    }
    assert(!pet_game_at(pet_game_count()) && !pet_game_find("missing"));
    puts("PASS: shared minigames: clocks, pause/release, catch-up, retry, one-shot rewards, clipping and registered renderers on all display sizes");
    return 0;
}
