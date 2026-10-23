#ifndef APPS_PET_GAME_H
#define APPS_PET_GAME_H

/* Deterministic game simulation. Logical coordinates are 0..1000 on any board.
 * No graphics, allocations, flash writes or clock dependencies. */
#include <stdbool.h>
#include <stdint.h>

#define PET_GAME_ITEMS 8
#define PET_GAME_DURATION_MS 60000
typedef enum { PET_GAME_SKI, PET_GAME_STARS } pet_game_kind_t;
typedef struct
{
    int x, y, lane;
    bool used, star;
} pet_game_item_t;
typedef struct
{
    pet_game_kind_t kind;
    uint32_t random, terrain_seed;
    int elapsed, remainder, spawn, invincible, points, lives;
    int x, y, velocity, lane;
    int world, height, speed, gap, combo, landing, airtime; /* Positions in sixteenths. */
    bool done, held, grounded, won;
    pet_game_item_t items[PET_GAME_ITEMS];
} pet_game_t;

void pet_game_start(pet_game_t *game, pet_game_kind_t kind, uint32_t seed);
/* Ski: positive holds, negative releases. Stars: moves one lane. */
void pet_game_input(pet_game_t *game, int direction);
void pet_game_hold(pet_game_t *game, bool held);
int pet_game_hill(const pet_game_t *game, int world);
int pet_game_slope(const pet_game_t *game, int world);
void pet_game_step(pet_game_t *game, int milliseconds);
int pet_game_score(const pet_game_t *game);
#endif
