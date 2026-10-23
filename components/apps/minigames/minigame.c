#include "minigame.h"
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

struct mg_session
{
    const mg_game_t *game;
    uint64_t last;
    int elapsed, remainder;
    bool active, held, done, claimed;
    mg_result_t result;
    /* malloc alignment is retained by sizeof(struct), including the state. */
    union { long double real; void *pointer; long long integer; } alignment;
};

void *mg_state(mg_session_t *s)
{
    return s ? (void *)(s + 1) : NULL;
}

uint32_t mg_random(uint32_t *state)
{
    uint32_t x = *state ? *state : 1;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

mg_session_t *mg_create(const mg_game_t *g, uint32_t seed, uint64_t now)
{
    if (!g || !g->id || !g->id[0] || !g->title || !g->start || !g->step ||
        !g->input || !g->draw || !g->result || !g->state_size || g->state_size > MG_MAX_STATE_BYTES)
        return NULL;
#ifdef ESP_PLATFORM
    mg_session_t *s = heap_caps_calloc(1, sizeof(*s) + g->state_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    mg_session_t *s = calloc(1, sizeof(*s) + g->state_size);
#endif
    if (!s) return NULL;
    s->game = g;
    mg_restart(s, seed, now);
    return s;
}

void mg_destroy(mg_session_t *s)
{
    free(s);
}

void mg_restart(mg_session_t *s, uint32_t seed, uint64_t now)
{
    if (!s) return;
    s->last = now;
    s->elapsed = s->remainder = 0;
    s->active = true;
    s->held = s->done = s->claimed = false;
    s->result = (mg_result_t){0};
    memset(mg_state(s), 0, s->game->state_size);
    s->game->start(mg_state(s), seed ? seed : 1);
}

void mg_input(mg_session_t *s, mg_input_kind_t kind, int x, int y, int direction)
{
    if (!s || s->done || !s->active) return;
    if (kind < MG_PRESS || kind > MG_CLICK) return;
    if (kind == MG_PRESS) s->held = true;
    if (kind == MG_RELEASE) s->held = false;
    mg_input_t input = {.kind = kind, .x = x < 0 ? 0 : x > 1000 ? 1000 : x,
                        .y = y < 0 ? 0 : y > 1000 ? 1000 : y,
                        .direction = direction < 0 ? -1 : direction > 0 ? 1 : 0, .held = s->held};
    s->game->input(mg_state(s), &input);
}

void mg_advance(mg_session_t *s, uint64_t now, bool active)
{
    if (!s) return;
    uint64_t dt = now >= s->last ? now - s->last : 0;
    s->last = now;
    if (!active || s->done)
    {
        if (s->active && !s->done)
        {
            mg_input_t release = {.kind = MG_RELEASE, .cancelled = true};
            s->held = false;
            s->game->input(mg_state(s), &release);
        }
        s->active = false;
        s->remainder = 0;
        return;
    }
    if (!s->active)
    {
        s->active = true;
        return;
    }
    /* A stall cannot create an unbounded catch-up loop in the UI task. */
    s->remainder += (int)(dt > 250 ? 250 : dt);
    while (s->remainder >= MG_STEP_MS && !s->done)
    {
        s->remainder -= MG_STEP_MS;
        s->elapsed += MG_STEP_MS;
        s->game->step(mg_state(s), MG_STEP_MS);
        s->done = s->game->result(mg_state(s), &s->result);
        if (s->elapsed >= MG_MAX_DURATION_MS) s->done = true;
        if (s->done)
        {
            s->result.score = s->result.score < 0 ? 0 : s->result.score > 100 ? 100 : s->result.score;
            s->held = false;
        }
    }
}

bool mg_take_result(mg_session_t *s, mg_result_t *result)
{
    if (!s || !result || !s->done || s->claimed) return false;
    *result = s->result;
    s->claimed = true;
    return true;
}
bool mg_done(const mg_session_t *s) { return s && s->done; }
int mg_elapsed(const mg_session_t *s) { return s ? s->elapsed : 0; }
