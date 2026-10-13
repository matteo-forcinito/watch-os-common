/* Ombra classifier tests: real advertisement layouts, malformed input, distance. */
#include "classify.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static ombra_adv_t s_adv, s_more;

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}

int ombra_tests(void)
{
    /* AirTag separated from its owner: Find My, 25-byte payload, status 0x10 (AirTag, battery full). */
    static const uint8_t airtag[] = {0x1E, 0xFF, 0x4C, 0x00, 0x12, 0x19, 0x10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                                     12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 0x01, 0x00};
    classify_adv(airtag, sizeof(airtag), &s_adv);
    CHECK(s_adv.kind == OMBRA_KIND_TRACKER && s_adv.tracker == OMBRA_TRACKER_AIRTAG);
    CHECK(s_adv.separated && s_adv.battery == 0 && s_adv.company == 0x004C);

    /* Same AirTag near its owner: 2-byte payload, low battery (status 0x90). */
    static const uint8_t nearby[] = {0x07, 0xFF, 0x4C, 0x00, 0x12, 0x02, 0x90, 0x00};
    classify_adv(nearby, sizeof(nearby), &s_adv);
    CHECK(s_adv.tracker == OMBRA_TRACKER_AIRTAG && !s_adv.separated && s_adv.battery == 2);

    /* Third-party Find My accessory (status bits 4-5 = 2). */
    static const uint8_t accessory[] = {0x07, 0xFF, 0x4C, 0x00, 0x12, 0x02, 0x20, 0x00};
    classify_adv(accessory, sizeof(accessory), &s_adv);
    CHECK(s_adv.tracker == OMBRA_TRACKER_FINDMY);

    /* Tile: 16-bit service 0xFEED. */
    static const uint8_t tile[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0xED, 0xFE};
    classify_adv(tile, sizeof(tile), &s_adv);
    CHECK(s_adv.tracker == OMBRA_TRACKER_TILE && s_adv.connectable_hint && s_adv.services[0] == 0xFEED);

    /* Samsung SmartTag: service data 0xFD5A. */
    static const uint8_t smarttag[] = {0x05, 0x16, 0x5A, 0xFD, 0x01, 0x02};
    classify_adv(smarttag, sizeof(smarttag), &s_adv);
    CHECK(s_adv.tracker == OMBRA_TRACKER_SMARTTAG);

    /* Google Find My Device tag in unwanted-tracking mode (frame 0x41). */
    static const uint8_t google[] = {0x05, 0x16, 0xAA, 0xFE, 0x41, 0x00};
    classify_adv(google, sizeof(google), &s_adv);
    CHECK(s_adv.tracker == OMBRA_TRACKER_GOOGLE && s_adv.separated);

    /* Eddystone UID is a beacon, not a tracker; power at 0 m = -20 dBm. */
    static const uint8_t eddystone[] = {0x05, 0x16, 0xAA, 0xFE, 0x00, 0xEC};
    classify_adv(eddystone, sizeof(eddystone), &s_adv);
    CHECK(s_adv.kind == OMBRA_KIND_BEACON && s_adv.has_tx_power && s_adv.tx_power == -61);

    /* iBeacon with measured power -59 dBm at 1 m. */
    static const uint8_t ibeacon[] = {0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                      13, 14, 15, 16, 0, 1, 0, 2, 0xC5};
    classify_adv(ibeacon, sizeof(ibeacon), &s_adv);
    CHECK(s_adv.kind == OMBRA_KIND_BEACON && s_adv.tx_power == -59);

    /* Keyboard: complete name + appearance 0x03C1. */
    static const uint8_t keyboard[] = {0x08, 0x09, 'M', 'X', ' ', 'K', 'e', 'y', 's', 0x03, 0x19, 0xC1, 0x03};
    classify_adv(keyboard, sizeof(keyboard), &s_adv);
    CHECK(same(s_adv.name, "MX Keys") && s_adv.kind == OMBRA_KIND_INPUT && s_adv.appearance == 0x03C1);

    /* Headphones recognised by name only. */
    static const uint8_t buds[] = {0x0D, 0x09, 'G', 'a', 'l', 'a', 'x', 'y', ' ', 'B', 'u', 'd', 's', '2'};
    classify_adv(buds, sizeof(buds), &s_adv);
    CHECK(s_adv.kind == OMBRA_KIND_AUDIO);

    /* Windows PC: Microsoft CDP beacon. */
    static const uint8_t windows[] = {0x05, 0xFF, 0x06, 0x00, 0x01, 0x09};
    classify_adv(windows, sizeof(windows), &s_adv);
    CHECK(s_adv.kind == OMBRA_KIND_COMPUTER && same(classify_company(s_adv.company), "Microsoft"));

    /* Malformed: a field longer than the buffer stops the walk, nothing is read past it. */
    static const uint8_t broken[] = {0x02, 0x01, 0x06, 0x1F, 0x09, 'A', 'B'};
    classify_adv(broken, sizeof(broken), &s_adv);
    CHECK(s_adv.name[0] == 0 && s_adv.kind == OMBRA_KIND_UNKNOWN);
    classify_adv(0, 0, &s_adv);
    CHECK(s_adv.company == 0xFFFF && s_adv.battery == -1);

    /* Control characters in names are replaced; long names are cut, still terminated. */
    static const uint8_t odd[] = {0x05, 0x09, 'a', 0x01, 'b', 0x7F};
    classify_adv(odd, sizeof(odd), &s_adv);
    CHECK(same(s_adv.name, "a?b?"));
    static uint8_t longname[2 + 40];
    longname[0] = 41;
    longname[1] = 0x09;
    for (int i = 0; i < 40; i++)
        longname[2 + i] = 'x';
    classify_adv(longname, sizeof(longname), &s_adv);
    CHECK(s_adv.name[sizeof(s_adv.name) - 1] == 0 && s_adv.name[sizeof(s_adv.name) - 2] == 'x');

    /* Scan response merge: tracker verdict kept, name added. */
    classify_adv(tile, sizeof(tile), &s_adv);
    static const uint8_t response[] = {0x05, 0x09, 'T', 'i', 'l', 'e'};
    classify_adv(response, sizeof(response), &s_more);
    classify_merge(&s_adv, &s_more);
    CHECK(s_adv.tracker == OMBRA_TRACKER_TILE && same(s_adv.name, "Tile"));
    classify_adv(response, sizeof(response), &s_more);
    classify_adv(airtag, sizeof(airtag), &s_adv);
    classify_merge(&s_more, &s_adv);
    CHECK(s_more.kind == OMBRA_KIND_TRACKER && s_more.separated);

    /* Distance: -59 dBm at 1 m reference -> 1 m; 22 dB more loss -> 10 m; clamps. */
    CHECK(classify_distance_cm(-59, -59) == 100);
    CHECK(classify_distance_cm(-81, -59) == 1000);
    CHECK(classify_distance_cm(-20, -59) == 4);
    CHECK(classify_distance_cm(-200, -59) == 151991);
    CHECK(same(classify_kind_name(OMBRA_KIND_TRACKER), "Tracker"));
    CHECK(same(classify_kind_name((ombra_kind_t)99), "Sconosciuto"));
    return 0;
}

#ifdef OMBRA_EMULATOR_ENTRY
__attribute__((naked)) void _start(void)
{
    __asm__ volatile("call ombra_tests\n ebreak\n");
}
#else
int main(void)
{
    return ombra_tests();
}
#endif
