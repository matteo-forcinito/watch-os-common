#include "pet_game.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    pet_game_t a, b;
    pet_game_start(&a, PET_GAME_SKI, 1);
    assert(a.grounded && a.y == 420 && a.speed == 400);
    assert(pet_game_slope(&a, 250) > 0 && pet_game_slope(&a, 750) < 0);
    for (int x = -2000; x < 2000; x++)
        assert(abs(pet_game_hill(&a, x + 1) - pet_game_hill(&a, x)) <= 2);
    a.world = 200 * 16;
    a.height = pet_game_hill(&a, 200) * 16;
    b = a;
    pet_game_hold(&a, true);
    for (int i = 0; i < 10; i++) { pet_game_step(&a, 16); pet_game_step(&b, 16); }
    assert(a.speed > b.speed);
    /* A fast release near the crest launches into the next valley. */
    pet_game_start(&a, PET_GAME_SKI, 1);
    a.world = 950 * 16;
    a.height = pet_game_hill(&a, 950) * 16;
    a.speed = 1000;
    for (int i = 0; i < 10 && a.grounded; i++) pet_game_step(&a, 16);
    assert(!a.grounded);
    /* Pressing at a flat crest adds weight, not a discontinuous brake. */
    pet_game_start(&a, PET_GAME_SKI, 1);
    pet_game_hold(&a, true);
    pet_game_step(&a, 16);
    assert(a.speed > 390 && a.grounded);
    /* The same downhill contact gains more momentum while pressed. */
    pet_game_start(&a, PET_GAME_SKI, 1);
    a.world = 200 * 16;
    a.height = pet_game_hill(&a, 200) * 16 - 1;
    a.grounded = false;
    a.airtime = 200;
    a.velocity = 1000;
    a.speed = 600;
    b = a;
    pet_game_hold(&a, true);
    pet_game_step(&a, 16);
    pet_game_step(&b, 16);
    assert(a.grounded && b.grounded && a.speed > b.speed);

    /* Force a genuine airborne landing on either side of a hill. */
    for (int downhill = 0; downhill < 2; downhill++)
    {
        pet_game_start(&a, PET_GAME_SKI, 1);
        a.world = (downhill ? 200 : 700) * 16;
        a.height = pet_game_hill(&a, a.world / 16) * 16 - 1;
        a.grounded = false;
        a.airtime = 200;
        a.velocity = 1000;
        a.speed = 600;
        pet_game_hold(&a, true);
        pet_game_step(&a, 16);
        assert(a.grounded);
        assert(downhill ? a.speed > 600 : a.speed < 300);
    }
    pet_game_start(&a, PET_GAME_SKI, 2);
    pet_game_hold(&a, true);
    for (int i = 0; i < 4000 && !a.done; i++) pet_game_step(&a, 16);
    assert(a.done && !a.won && a.lives == 0); /* Holding forever loses. */

    pet_game_start(&a, PET_GAME_SKI, 2);
    for (int i = 0; i < 4000 && !a.done; i++)
    {
        int phase = a.world / 16 % 1000;
        pet_game_hold(&a, a.grounded ? phase < 500 : phase > 100 && phase < 450 && a.velocity > 0);
        pet_game_step(&a, 16);
    }
    printf("timed run: %d ms, score %d, won %d\n", a.elapsed, pet_game_score(&a), a.won);
    fflush(stdout);
    assert(a.won);

    pet_game_start(&a, PET_GAME_STARS, 4);
    a.spawn = 999999;
    for (int i = 0; i < 10; i++) pet_game_input(&a, -1);
    assert(a.lane == 0 && a.x == 250);
    a.items[0] = (pet_game_item_t){.used = true, .star = true, .lane = 0, .x = 250, .y = 730};
    a.items[1] = (pet_game_item_t){.used = true, .lane = 2, .x = 750, .y = 730};
    pet_game_step(&a, 16);
    assert(a.points == 5 && a.lives == 3);
    for (int i = 0; i < 10; i++) pet_game_input(&a, 1);
    assert(a.lane == 2 && a.x == 750);
    pet_game_step(&a, 16);
    assert(a.lives == 2);

    for (int kind = 0; kind < 2; kind++)
        for (unsigned seed = 1; seed <= 100; seed++)
        {
            pet_game_start(&a, (pet_game_kind_t)kind, seed);
            pet_game_start(&b, (pet_game_kind_t)kind, seed);
            for (int i = 0; i < 2000; i++)
            {
                if (i % 20 == 0)
                {
                    pet_game_input(&a, i % 40 ? -1 : 1);
                    pet_game_input(&b, i % 40 ? -1 : 1);
                }
                pet_game_step(&a, 32);
                pet_game_step(&b, 16);
                pet_game_step(&b, 16);
                assert(memcmp(&a, &b, sizeof(a)) == 0);
                assert(a.lives >= 0 && a.lives <= 3 && a.lane >= 0 && a.lane <= 2);
                assert(pet_game_score(&a) >= 0 && pet_game_score(&a) <= 100);
                if (a.done) break;
            }
        }
    pet_game_start(&a, PET_GAME_STARS, 99);
    a.spawn = 999999;
    for (int i = 0; i < 4000; i++) pet_game_step(&a, 16);
    assert(a.done && a.elapsed == PET_GAME_DURATION_MS);
    b = a;
    pet_game_step(&a, 1000);
    pet_game_input(&a, 1);
    assert(memcmp(&a, &b, sizeof(a)) == 0);
    a.points = 10000;
    assert(pet_game_score(&a) == 100);
    puts("PASS: pet games: hills, slope, pumping, launch, landing, avalanche, lanes, collection, 200 deterministic runs, duration and score bounds");
    return 0;
}
