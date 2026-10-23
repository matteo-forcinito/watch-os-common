#include "../components/apps/minigames/games/adventures.c"
#include <assert.h>
#include <limits.h>
int main(void)
{
    adventure_t *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
    assert(a && b);
    archer_start(a, 1);
    damage_player(&a->m);
    assert(a->m.hp == 3);
    damage_player(&a->m);
    assert(a->m.hp == 3);
    for (int n = 0; n < 80; n++) step(a, 16);
    a->m.invulnerable = 0;
    damage_player(&a->m);
    assert(a->m.hp == 2);
    for (int mode = 0; mode < 3; mode++)
    {
        for (unsigned seed = 1; seed <= 10; seed++)
        {
            memset(a, 0, sizeof(*a)); memset(b, 0, sizeof(*b));
            init(a, seed, mode); init(b, seed, mode);
            for (int n = 0; n < 225000; n++)
            {
                if (n % 127 == 0)
                {
                    mg_input_t e = {.kind = MG_PRESS, .x = n % 1000, .y = n * 7 % 1000};
                    input(a, &e);
                }
                if (n % 127 == 15)
                {
                    mg_input_t e = {.kind = MG_RELEASE}; input(a, &e);
                }
                step(a, 16);
                if (n % 4096 == 0)
                {
                    uint8_t blob[sizeof(adventure_model_t)];
                    assert(save(a, blob, sizeof(blob)));
                    assert(load(b, blob, sizeof(blob)));
                    assert(b->m.stage == a->m.stage && b->m.bank == a->m.bank);
                    assert(b->m.hp == a->m.hp && !b->m.held);
                }
                assert(a->m.hp >= 0 && a->m.hp <= max_hp(&a->m));
                assert(a->m.air >= 0 && a->m.air <= 100000);
                assert(a->m.clock >= 0 && a->m.clock < 60000);
            }
            int old_stage = a->m.stage, old_weapon = a->m.weapon;
            clear_room(&a->m);
            a->m.bank = 1000;
            mg_input_t pick = {.kind = MG_PRESS, .x = 100}; input(a, &pick);
            pick.kind = MG_RELEASE; input(a, &pick);
            assert(a->m.stage == old_stage + 1 && a->m.weapon == bound(old_weapon + 1, 1, 30));
            a->m.hp = 1; a->m.invulnerable = 0;
            damage_player(&a->m);
            int weapon = a->m.weapon;
            for (int n = 0; n < 120; n++) step(a, 16);
            assert(!a->m.rescue && a->m.hp == max_hp(&a->m) && a->m.weapon == weapon);
        }
    }
    uint8_t blob[sizeof(adventure_model_t)];
    assert(save(a, blob, sizeof(blob)));
    ((adventure_model_t *)blob)->enemies[0].x = INT_MIN;
    assert(!load(b, blob, sizeof(blob)));
    free(a); free(b);
    puts("PASS: 30 adventure hour-long soaks, checkpoint reloads, upgrades, rescue and invalid-state rejection");
    return 0;
}
