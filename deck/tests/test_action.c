/* Deck action parser tests. */
#include "action.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static deck_action_t s_action;

int deck_tests(void)
{
    /* One chord with modifiers. */
    CHECK(deck_action_parse("ctrl+shift+m", &s_action) && s_action.count == 1);
    CHECK(s_action.steps[0].kind == DECK_STEP_KEY && s_action.steps[0].modifiers == 0x03 &&
          s_action.steps[0].key == 0x10);

    /* Case-insensitive names, F keys, digits, named keys. */
    CHECK(deck_action_parse("Win+D", &s_action) && s_action.steps[0].modifiers == 0x08 && s_action.steps[0].key == 0x07);
    CHECK(deck_action_parse("shift+f5", &s_action) && s_action.steps[0].key == 0x3E);
    CHECK(deck_action_parse("f13", &s_action) && s_action.steps[0].key == 0x68);
    CHECK(deck_action_parse("f24", &s_action) && s_action.steps[0].key == 0x73);
    CHECK(deck_action_parse("ctrl+0", &s_action) && s_action.steps[0].key == 0x27);
    CHECK(deck_action_parse("ctrl+win+left", &s_action) && s_action.steps[0].modifiers == 0x09 &&
          s_action.steps[0].key == 0x50);
    CHECK(deck_action_parse("ctrl+shift+esc", &s_action) && s_action.steps[0].key == 0x29);
    CHECK(deck_action_parse("ctrl+kpplus", &s_action) && s_action.steps[0].key == 0x57);

    /* Chords in sequence (VS Code style). */
    CHECK(deck_action_parse("ctrl+k ctrl+c", &s_action) && s_action.count == 2);
    CHECK(s_action.steps[1].modifiers == 0x01 && s_action.steps[1].key == 0x06);

    /* Steps with waits and text: text keeps its spaces. */
    static const char launch[] = "win+r; wait:300; type:wt -p Ubuntu; enter";
    CHECK(deck_action_parse(launch, &s_action) && s_action.count == 4);
    CHECK(s_action.steps[1].kind == DECK_STEP_WAIT && s_action.steps[1].value == 300);
    CHECK(s_action.steps[2].kind == DECK_STEP_TEXT && s_action.steps[2].length == 12);
    CHECK(launch[s_action.steps[2].offset] == 'w' && launch[s_action.steps[2].offset + 11] == 'u');
    CHECK(s_action.steps[3].key == 0x28 && s_action.steps[3].modifiers == 0);

    /* Media and wheel. */
    CHECK(deck_action_parse("media:mute", &s_action) && s_action.steps[0].kind == DECK_STEP_MEDIA &&
          s_action.steps[0].value == 0x00E2);
    CHECK(deck_action_parse("media:volup;media:volup", &s_action) && s_action.count == 2);
    CHECK(deck_action_parse("wheel:-3", &s_action) && s_action.steps[0].wheel == -3);

    /* Modifiers alone are a valid chord (e.g. tap the Windows key). */
    CHECK(deck_action_parse("win", &s_action) && s_action.steps[0].key == 0 && s_action.steps[0].modifiers == 0x08);

    /* Errors carry a position. */
    CHECK(!deck_action_parse("ctrl+banana", &s_action) && s_action.error_at == 5);
    CHECK(!deck_action_parse("ctrl+", &s_action) && s_action.error_at == 4);
    CHECK(!deck_action_parse("a+b", &s_action) && s_action.error_at == 2);
    CHECK(!deck_action_parse("media:fly", &s_action) && s_action.error_at == 6);
    CHECK(!deck_action_parse("wait:9000", &s_action));
    CHECK(!deck_action_parse("wheel:0", &s_action));
    CHECK(!deck_action_parse("type:", &s_action));
    CHECK(!deck_action_parse("  ;  ", &s_action) && s_action.error != 0);
    CHECK(!deck_action_parse(0, &s_action));
    CHECK(!deck_action_parse("+a", &s_action) && s_action.error_at == 0);

    /* No more than DECK_STEPS steps. */
    static const char many[] = "a;a;a;a;a;a;a;a;a;a;a;a;a;a;a;a;a;a;a;a;a;a;a;a;a";
    CHECK(!deck_action_parse(many, &s_action));
    return 0;
}

#ifdef DECK_EMULATOR_ENTRY
__attribute__((naked)) void _start(void)
{
    /* Full-size ebreak: the runner looks for the 4-byte instruction. */
    __asm__ volatile(".option push\n .option norvc\n call deck_tests\n ebreak\n .option pop\n");
}
#else
int main(void)
{
    return deck_tests();
}
#endif
