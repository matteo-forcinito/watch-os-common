#include "pet_game.h"
#include <stdlib.h>
#include <string.h>

static uint32_t random_next(pet_game_t *g)
{
    uint32_t x = g->random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g->random = x;
}

/* Smooth, continuous hills with zero slope at each crest and valley. */
static int hill_parts(const pet_game_t *g, int world, int *u, int *sign)
{
    int hill = world / 1000, phase = world % 1000;
    if (phase < 0) phase += 1000, hill--;
    uint32_t hash = (uint32_t)hill * 2654435761u ^ g->terrain_seed;
    hash ^= hash >> 16;
    *sign = phase <= 500 ? 1 : -1;
    *u = phase <= 500 ? phase : 1000 - phase;
    return 310 + (int)(hash % 71);
}

int pet_game_hill(const pet_game_t *g, int world)
{
    int u, sign, a = hill_parts(g, world, &u, &sign);
    return 420 + (int)((int64_t)a * u * u * (1500 - 2 * u) / 125000000);
}

int pet_game_slope(const pet_game_t *g, int world)
{
    int u, sign, a = hill_parts(g, world, &u, &sign);
    return sign * (int)((int64_t)a * 6 * u * (500 - u) * 1000 / 125000000);
}

/* Subpixel terrain avoids false takeoffs from integer-height stair steps. */
static int hill_height(const pet_game_t *g, int world)
{
    int u, sign;
    int a = hill_parts(g, world / 16, &u, &sign);
    int phase = world % 16000;
    if (phase < 0) phase += 16000;
    int sub = phase <= 8000 ? phase : 16000 - phase;
    return 420 * 16 + (int)((int64_t)a * sub * sub * (24000 - 2 * sub) / 32000000000LL);
}

void pet_game_hold(pet_game_t *g, bool held)
{
    if (!g->done && g->kind == PET_GAME_SKI) g->held = held;
}

static void ski_step(pet_game_t *g)
{
    int slope = pet_game_slope(g, g->world / 16);
    if (g->landing > 0) g->landing -= 16;
    if (g->grounded)
    {
        /* Project gravity onto the tangent: pressing adds weight, rather
         * than applying an arbitrary brake as soon as the slope changes. */
        int gravity = g->held ? 2000 : 650;
        int accel = (int)((int64_t)gravity * slope * 1000 / (1000000 + slope * slope)) - 20 - g->speed / 40;
        g->speed += accel * 16 / 1000;
        if (g->speed < 120) g->speed = 120;
        if (g->speed > 1250) g->speed = 1250;
        g->velocity = slope * g->speed / 1000;
    }
    g->world += g->speed * 16 * 16 / 1000;
    int floor = hill_height(g, g->world);
    int next = g->height + (g->velocity + 500 * 16 / 1000) * 16 * 16 / 1000;
    /* Leaving the ground follows the tangent and the curve, at any crest. */
    if (g->grounded && (g->held || next >= floor - 2))
        g->height = floor;
    else
    {
        g->grounded = false;
        g->airtime += 16;
        g->velocity += (g->held ? 1800 : 500) * 16 / 1000;
        g->height += g->velocity * 16 * 16 / 1000;
        if (g->height >= floor)
        {
            slope = pet_game_slope(g, g->world / 16);
            if (g->airtime >= 128)
            {
                int projected = (int)(((int64_t)g->speed * 1000000 +
                                      (int64_t)g->velocity * slope * 1000) / (1000000 + slope * slope));
                g->speed = projected < 120 ? 120 : projected > 1250 ? 1250 : projected;
            }
            if (slope > 150 && g->airtime >= 128)
            {
                g->speed += (g->held ? 60 : 0) + g->combo * 6;
                if (g->speed > 1250) g->speed = 1250;
                if (g->combo < 8) g->combo++;
                g->points += 3;
                g->landing = 1000;
            }
            else if (slope < -150 && g->airtime >= 128)
            {
                g->combo = 0;
                g->landing = 1000;
            }
            g->grounded = true;
            g->airtime = 0;
            g->height = floor;
        }
    }
    g->y = g->height / 16;
    g->gap += (g->speed - (330 + g->elapsed / 400)) * 16 * 16 / 1000;
    if (g->gap > 1400 * 16) g->gap = 1400 * 16;
    if (g->gap <= 90 * 16) g->lives = 0, g->done = true;
}

void pet_game_start(pet_game_t *g, pet_game_kind_t kind, uint32_t seed)
{
    memset(g, 0, sizeof(*g));
    g->kind = kind;
    g->random = seed ? seed : 1;
    g->terrain_seed = g->random;
    g->grounded = true;
    g->speed = 400;
    g->gap = 800 * 16;
    g->lives = 3;
    g->lane = 1;
    g->x = kind == PET_GAME_SKI ? 300 : 500;
    g->y = kind == PET_GAME_SKI ? 420 : 780;
    g->height = g->y * 16;
    g->spawn = kind == PET_GAME_SKI ? 1500 : 900;
}

void pet_game_input(pet_game_t *g, int direction)
{
    if (g->done)
        return;
    if (g->kind == PET_GAME_SKI)
    {
        pet_game_hold(g, direction > 0);
    }
    else
    {
        g->lane += direction < 0 ? -1 : direction > 0 ? 1 : 0;
        if (g->lane < 0) g->lane = 0;
        if (g->lane > 2) g->lane = 2;
        g->x = 250 + g->lane * 250;
    }
}

static void step(pet_game_t *g)
{
    g->elapsed += 16;
    if (g->invincible > 0)
        g->invincible -= 16;
    if (g->kind == PET_GAME_SKI)
    {
        ski_step(g);
        if (!g->done && g->elapsed >= PET_GAME_DURATION_MS) g->won = g->done = true;
        return;
    }
    g->spawn -= 16;
    if (g->spawn <= 0)
    {
        for (int i = 0; i < PET_GAME_ITEMS; i++)
            if (!g->items[i].used)
            {
                pet_game_item_t *p = &g->items[i];
                p->used = true;
                p->star = (random_next(g) % 4) != 3u;
                p->lane = (int)(random_next(g) % 3);
                p->x = 250 + p->lane * 250;
                p->y = -80;
                break;
            }
        g->spawn = 550 + (int)(random_next(g) % 300);
    }
    int speed = 240 + g->elapsed / 600;
    for (int i = 0; i < PET_GAME_ITEMS; i++)
    {
        pet_game_item_t *p = &g->items[i];
        if (!p->used)
            continue;
        p->y += speed * 16 / 1000;
        bool overlap = p->lane == g->lane && abs(p->y - (g->y - 45)) < 65;
        if (overlap)
        {
            if (p->star)
                g->points += 5;
            else if (g->invincible <= 0)
            {
                g->lives--;
                g->invincible = 1000;
            }
            p->used = false;
        }
        else if (p->x < -100 || p->y > 1100)
        {
            p->used = false;
        }
    }
    if (g->lives <= 0 || g->elapsed >= PET_GAME_DURATION_MS)
        g->done = true;
}

void pet_game_step(pet_game_t *g, int milliseconds)
{
    if (milliseconds <= 0 || g->done)
        return;
    /* Bound work after a delayed UI frame; screen-off time is never credited. */
    if (milliseconds > 250) milliseconds = 250;
    g->remainder += milliseconds;
    while (g->remainder >= 16 && !g->done)
    {
        g->remainder -= 16;
        step(g);
    }
    if (g->done)
        g->remainder = 0;
}

int pet_game_score(const pet_game_t *g)
{
    int score = g->points + g->elapsed / 2000;
    return score > 100 ? 100 : score < 0 ? 0 : score;
}
