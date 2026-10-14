/* Copertura judgement tests. Returns the failing line, 0 if all pass. */
#include "advice.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int copertura_tests(void)
{
    /* Grades: signal first, then loss and latency push it down. */
    CHECK(cp_grade(-48, 8, 0) == CP_A && cp_grade(-60, 10, 0) == CP_B && cp_grade(-68, 10, 0) == CP_C);
    CHECK(cp_grade(-75, 10, 0) == CP_D && cp_grade(-82, 10, 0) == CP_E && cp_grade(-90, 10, 0) == CP_F);
    CHECK(cp_grade(-48, 8, 6) == CP_B && cp_grade(-48, 8, 30) == CP_C);
    CHECK(cp_grade(-48, 70, 0) == CP_B && cp_grade(-48, 200, 0) == CP_C && cp_grade(-90, 300, 50) == CP_F);
    CHECK(cp_grade(-48, 0, 0) == CP_A);   /* unknown ping does not penalise */
    CHECK(cp_grade_letter(CP_A)[0] == 'A' && cp_grade_letter(CP_F)[0] == 'F');
    CHECK(cp_signal_percent(-90) == 0 && cp_signal_percent(-40) == 100 && cp_signal_percent(-65) == 50);
    CHECK(cp_signal_percent(-20) == 100 && cp_signal_percent(-100) == 0);

    /* Channels: a strong neighbour on 6 and a weaker one on 1, we are on 6. */
    cp_neighbour_t n[] = {{6, -45}, {7, -60}, {1, -75}, {11, -88}};
    cp_channels_t c;
    cp_channel_advice(n, 4, 6, &c);
    CHECK(c.score[6] > c.score[1] && c.score[6] > c.score[11] && c.score[13] < c.score[6]);
    CHECK(c.score[11] < c.score[1]);
    CHECK(c.best == 13 || c.best == 11);
    CHECK(c.current == 6 && c.worth_changing);
    /* Overlap: channel 4 is touched by 6 (two steps) but 12 is not. */
    CHECK(c.score[4] > 0 && c.score[12] < c.score[4]);
    /* Empty air: stay. */
    cp_channel_advice(n, 0, 6, &c);
    CHECK(!c.worth_changing && c.best_score == 0 && c.current_score == 0);
    /* Small gains are not worth a change. */
    cp_neighbour_t calm[] = {{6, -80}, {1, -82}};
    cp_channel_advice(calm, 2, 6, &c);
    CHECK(!c.worth_changing);

    /* Repeater: rooms at -48 (router), -61, -66, -79 (weak), -88 (weak). */
    cp_spot_t spots[] = {{-48, false}, {-61, false}, {-66, false}, {-79, true}, {-88, true}};
    CHECK(cp_repeater_spot(spots, 5) == 1);
    cp_spot_t strong[] = {{-48, false}, {-55, false}};
    CHECK(cp_repeater_spot(strong, 2) == -1);       /* no weak room: no repeater */
    cp_spot_t far[] = {{-75, false}, {-85, true}};
    CHECK(cp_repeater_spot(far, 2) == -1);          /* nowhere strong enough */
    CHECK(cp_repeater_spot(spots, 0) == -1);

    /* Finder heat. */
    CHECK(cp_finder_heat(1, -62, -40) == 100 && cp_finder_heat(1, -72, -40) == 50 && cp_finder_heat(1, -90, -40) == 0);
    CHECK(cp_finder_heat(1, -52, -40) == 50);
    CHECK(cp_finder_heat(0, -50, -50) == 100 && cp_finder_heat(0, -45, -50) == 100 && cp_finder_heat(0, -65, -50) == 0);
    CHECK(cp_finder_heat(0, -56, -50) == 60);
    return 0;
}

#ifdef COPERTURA_EMULATOR_ENTRY
__attribute__((naked)) void _start(void)
{
    /* Full-size ebreak: the runner looks for the 4-byte instruction. */
    __asm__ volatile(".option push\n .option norvc\n call copertura_tests\n ebreak\n .option pop\n");
}
#else
#include <stdio.h>
int main(void)
{
    int line = copertura_tests();
    if (line) printf("FAIL at line %d\n", line);
    return line;
}
#endif
