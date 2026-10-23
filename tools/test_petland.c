#include "../petland/main/world.c"
#include <assert.h>
static void walk(rpg_t *s, int x, int y)
{
    /* All landmarks are connected by real traversable roads. */
    while (s->m.x != 12) move(s, s->m.x < 12 ? 0 : 2);
    while (s->m.y != y) move(s, s->m.y < y ? 1 : 3);
    while (s->m.x != x) move(s, s->m.x < x ? 0 : 2);
    assert(!s->m.mode && s->m.x == x && s->m.y == y);
}
static void win_battle(rpg_t *s)
{
    int moves = 0;
    int boss = s->m.boss;
    while (s->m.mode == 1 && moves++ < 100)
    {
        if (s->m.hp < hp_max(&s->m) * 2 / 3 && s->m.potions && s->m.intent != 3) combat(s, 3);
        else if (s->m.intent == 3 || s->m.intent == 1) combat(s, 2);
        else if (s->m.intent == 2 && s->m.mana >= 3) combat(s, 1);
        else combat(s, 0);
    }
    if (moves >= 100 || s->m.mode != 4) fprintf(stderr, "BOSS %d failed: moves=%d mode=%d level=%d hp=%d enemy=%d potions=%d\n", boss, moves, s->m.mode, s->m.level, s->m.hp, s->m.enemy_hp, s->m.potions);
    assert(moves < 100 && s->m.mode == 4);
    take_drop(s, !s->m.gear[1] ? 1 : !s->m.gear[2] ? 2 : 3);
    assert(!s->m.mode);
}
static void test_collection(void)
{
    rpg_t a = {0}, b = {0};
    wild_start(&a, 42); start(&b, 42);
    uint8_t blob[sizeof(rpg_model_t)];
    assert(a.m.mode == 5 && save(&a, blob, sizeof(blob)));
    assert(!load(&b, blob, sizeof(blob))); /* Campaigns cannot share saves. */
    choose_starter(&a, 1);
    assert(a.m.active_pet == 3 && a.m.caught == (1 << 3));
    assert(effectiveness(0, 2) == 200 && effectiveness(2, 0) == 50);
    assert(effectiveness(1, 0) == 200 && effectiveness(0, 0) == 100);
    int successes = 0;
    for (unsigned seed = 1; seed <= 200; seed++)
    {
        wild_start(&a, seed); choose_starter(&a, 1);
        battle(&a, 0); a.m.enemy = 1; a.m.enemy_hp = 1; a.m.rare = 0;
        combat(&a, 4);
        assert(a.m.capsules == 7);
        if (a.m.mode == 4)
        {
            successes++;
            assert((a.m.caught & 1) && a.m.party[1] == 0 && a.m.creature_hp[0]);
            take_drop(&a, 1);
            battle(&a, 0);
            a.m.intent = 2; /* A switch on a charging turn takes no damage. */
            a.m.hp = 5;
            combat(&a, 5);
            assert(a.m.active_pet == 0 && a.m.creature_hp[3] == 5);
            a.m.intent = 3; a.m.hp = 1;
            combat(&a, 2);
            assert(a.m.active_pet == 3 && a.m.hp == 5 && !a.m.creature_hp[0]);
            assert(save(&a, blob, sizeof(blob)));
            wild_start(&b, seed);
            assert(load(&b, blob, sizeof(blob)) && b.m.active_pet == 3 && !b.m.creature_hp[0]);
        }
    }
    assert(successes > 120 && successes < 195); /* Weakening helps, never guarantees. */
    start(&a, 7);
    a.m.beast_xp[6] = 48; assert(beast_form(&a.m, 6) == 0);
    a.m.seals = 1; assert(beast_form(&a.m, 6) == 1);
    a.m.beast_xp[6] = 132; assert(beast_form(&a.m, 6) == 1);
    a.m.seals = 15; assert(beast_form(&a.m, 6) == 2);
    /* Charging enemies reward interruption; blindly attacking suffers the heavy turn. */
    start(&a, 9); battle(&a, 1); a.m.intent = 2; b = a;
    combat(&a, 1); combat(&b, 0);
    assert(a.m.intent == 0 && b.m.intent == 3);
    combat(&a, 2); combat(&b, 0);
    assert(a.m.hp > b.m.hp);
    victory(&a);
    int drop = a.m.drop;
    mg_input_t touch = {.kind = MG_PRESS, .x = 400};
    input(&a, &touch);
    assert(a.m.mode == 4 && a.m.drop == drop && a.m.choice == 1);
    touch.kind = MG_RELEASE; touch.cancelled = true;
    input(&a, &touch);
    assert(a.m.mode == 4 && !a.m.gear[1]);
    touch.kind = MG_PRESS; touch.cancelled = false; input(&a, &touch);
    touch.kind = MG_RELEASE; input(&a, &touch);
    assert(!a.m.mode && a.m.gear[1] == drop);
}
static void test_renderers(void)
{
    const int dimensions[][2] = {{368,448},{466,466},{360,360},{412,412},{480,480}};
    static const uint8_t actor[32 * 32] = {0};
    static const uint16_t palette[16] = {0};
    for (size_t board = 0; board < sizeof(dimensions) / sizeof(dimensions[0]); board++)
    {
        int w = dimensions[board][0], h = dimensions[board][1] * 45 / 100;
        size_t bytes = (size_t)w * h * 2;
        uint8_t *memory = malloc(bytes + 32); assert(memory);
        memset(memory, 0xA5, bytes + 32);
        mg_canvas_t canvas = {.pixels = memory + 16, .width = w, .height = h, .stride = w * 2,
            .actor_grid = actor, .actor_palette = palette, .actor_size = 32};
        rpg_t state = {0}; start(&state, 123);
        battle(&state, 1);
        for (int mode = 0; mode <= 6; mode++)
        {
            state.m.mode = mode; state.m.choice = 0; state.redraw = true;
            mg_canvas_begin(&canvas); draw(&state, &canvas);
            for (int i = 0; i < 16; i++) assert(memory[i] == 0xA5 && memory[16 + bytes + i] == 0xA5);
        }
        free(memory);
    }
}
int main(void)
{
    test_collection();
    test_renderers();
    for (unsigned seed = 1; seed <= 24; seed++)
    {
        rpg_t a = {0}, b = {0};
        start(&a, seed); start(&b, seed);
        interact(&a);
        assert(!a.m.mode && a.m.quest);
        interact(&a);
        assert(a.m.mode == 2 && a.m.quest);
        a.m.mode = 0;
        for (int shrine = 0; shrine < 4; shrine++)
        {
            /* Prepare a defensive role at the real village camp. */
            if (!a.m.mode) interact(&a);
            shop(&a, 3);
            while (a.m.role != 1) camp(&a, 1);
            camp(&a, 4);
            walk(&a, s_shrines[shrine][0], s_shrines[shrine][1]);
            interact(&a);
            assert(a.m.mode == 1 && a.m.boss == shrine + 1);
            uint8_t blob[sizeof(rpg_model_t)];
            assert(save(&a, blob, sizeof(blob)) && load(&b, blob, sizeof(blob)));
            assert(b.m.enemy_hp == a.m.enemy_hp && b.m.x == a.m.x && b.m.seals == a.m.seals);
            win_battle(&a);
            assert(a.m.seals & (1 << shrine));
            walk(&a, 12, 12); interact(&a); a.m.mode = 0;
        }
        assert(a.m.seals == 15 && a.m.boat);
        walk(&a, 46, 46); interact(&a);
        assert(a.m.boss == 5);
        win_battle(&a);
        assert(a.m.chapter == 2 && !a.m.seals);
        assert(!load(&b, &a.m, sizeof(a.m) - 1));
        a.m.hp = -1;
        assert(!load(&b, &a.m, sizeof(a.m)));
    }
    puts("PASS: Petland captures, independent campaigns, party HP/KO, evolutions, elemental strategy and 24 complete worlds");
    return 0;
}
