/* Giornata calendar reader tests. Returns the failing line, 0 if all pass. */
#include "ics.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

#define MAX_OUT 64

static ics_occurrence_t s_out[MAX_OUT];
static int s_count;
static ics_event_t s_deferred[8];
static int s_deferred_count;
static ics_reader_t s_reader;

static bool collect(const ics_occurrence_t *o, void *ctx)
{
    (void)ctx;
    if (s_count < MAX_OUT)
    {
        s_out[s_count++] = *o;
    }
    return true;
}

static void defer(const ics_event_t *e, void *ctx)
{
    (void)ctx;
    if (s_deferred_count < 8)
    {
        s_deferred[s_deferred_count++] = *e;
    }
}

/* Rome in October 2026: UTC+2 until the 25th at 01:00 UTC, then UTC+1. */
static ics_time_t rome(int64_t utc)
{
    int64_t change = ics_days_from_civil(2026, 10, 25) * 86400 + 3600;
    return utc + (utc < change ? 7200 : 3600);
}

static int length(const char *s)
{
    int n = 0;
    while (s[n] != '\0')
    {
        n++;
    }
    return n;
}

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static ics_time_t at(int y, int mo, int d, int h, int mi)
{
    return ics_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60;
}

static int find(ics_time_t start, const char *title)
{
    for (int i = 0; i < s_count; i++)
    {
        if (s_out[i].start == start && eq(s_out[i].title, title))
        {
            return i;
        }
    }
    return -1;
}

static int count_title(const char *title)
{
    int n = 0;
    for (int i = 0; i < s_count; i++)
    {
        n += eq(s_out[i].title, title);
    }
    return n;
}

static void run(const char *text, ics_time_t from, ics_time_t to, bool with_defer, int chunk)
{
    s_count = 0;
    s_deferred_count = 0;
    ics_init(&s_reader, from, to, rome, collect, with_defer ? defer : 0, 0);
    int n = length(text);
    for (int i = 0; i < n; i += chunk)
    {
        ics_feed_text(&s_reader, text + i, (size_t)(n - i < chunk ? n - i : chunk));
    }
    ics_finish(&s_reader);
    for (int i = 0; i < s_deferred_count; i++)
    {
        ics_expand(&s_reader, &s_deferred[i]);
    }
}

static const char CALENDAR[] =
    "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Google Inc//Google Calendar 70.9054//EN\r\n"
    "BEGIN:VTIMEZONE\r\nTZID:Europe/Rome\r\nBEGIN:DAYLIGHT\r\nDTSTART:19700329T020000\r\nEND:DAYLIGHT\r\nEND:VTIMEZONE\r\n"
    /* single, local time with TZID, folded title, escaped comma */
    "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Rome:20261014T090000\r\nDTEND;TZID=Europe/Rome:20261014T100000\r\n"
    "UID:single@x\r\nSUMMARY:Riunione\\, team\r\n  prodotto\r\nLOCATION:Sala 2\r\n"
    "DESCRIPTION:" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
    " bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\r\n"
    "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT10M\r\nDESCRIPTION:Promemoria\r\nEND:VALARM\r\nEND:VEVENT\r\n"
    /* UTC time: 14:30Z on the 15th is 16:30 in Rome (summer time) */
    "BEGIN:VEVENT\r\nDTSTART:20261015T143000Z\r\nDURATION:PT45M\r\nUID:utc@x\r\nSUMMARY:Chiamata\r\nEND:VEVENT\r\n"
    /* all day */
    "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20261016\r\nDTEND;VALUE=DATE:20261017\r\nUID:day@x\r\nSUMMARY:Compleanno 🎂\r\nEND:VEVENT\r\n"
    /* weekly Mon/Wed at 8:00 local, until the 28th, one exception, one moved instance after the master */
    "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Rome:20261005T080000\r\nDTEND;TZID=Europe/Rome:20261005T083000\r\n"
    "RRULE:FREQ=WEEKLY;BYDAY=MO,WE;UNTIL=20261028T215959Z\r\nEXDATE;TZID=Europe/Rome:20261019T080000\r\n"
    "UID:gym@x\r\nSUMMARY:Palestra\r\nEND:VEVENT\r\n"
    "BEGIN:VEVENT\r\nRECURRENCE-ID;TZID=Europe/Rome:20261021T080000\r\nDTSTART;TZID=Europe/Rome:20261021T190000\r\n"
    "DTEND;TZID=Europe/Rome:20261021T193000\r\nUID:gym@x\r\nSUMMARY:Palestra\r\nEND:VEVENT\r\n"
    /* cancelled instance of the same series */
    "BEGIN:VEVENT\r\nRECURRENCE-ID;TZID=Europe/Rome:20261026T080000\r\nDTSTART;TZID=Europe/Rome:20261026T080000\r\n"
    "UID:gym@x\r\nSTATUS:CANCELLED\r\nSUMMARY:Palestra\r\nEND:VEVENT\r\n"
    /* monthly second Tuesday, 3 times */
    "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Rome:20260908T180000\r\nDTEND;TZID=Europe/Rome:20260908T190000\r\n"
    "RRULE:FREQ=MONTHLY;BYDAY=2TU;COUNT=3\r\nUID:club@x\r\nSUMMARY:Club\r\nEND:VEVENT\r\n"
    /* monthly last day */
    "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20260131\r\nRRULE:FREQ=MONTHLY;BYMONTHDAY=-1\r\nUID:rent@x\r\nSUMMARY:Affitto\r\nEND:VEVENT\r\n"
    /* yearly 29 February */
    "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20240229\r\nRRULE:FREQ=YEARLY\r\nUID:leap@x\r\nSUMMARY:Bisestile\r\nEND:VEVENT\r\n"
    /* daily every 2 days, 4 times, starting before the window */
    "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Rome:20261008T070000\r\nDTEND;TZID=Europe/Rome:20261008T071500\r\n"
    "RRULE:FREQ=DAILY;INTERVAL=2;COUNT=6\r\nUID:pill@x\r\nSUMMARY:Pillola\r\nEND:VEVENT\r\n"
    /* cancelled single */
    "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Rome:20261014T120000\r\nUID:gone@x\r\nSTATUS:CANCELLED\r\nSUMMARY:Annullato\r\nEND:VEVENT\r\n"
    /* outside the window */
    "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Rome:20251014T120000\r\nUID:old@x\r\nSUMMARY:Vecchio\r\nEND:VEVENT\r\n"
    "END:VCALENDAR\r\n";

int giornata_tests(void)
{
    /* Dates and helpers. */
    CHECK(ics_days_from_civil(1970, 1, 1) == 0 && ics_weekday(0) == 3);
    CHECK(ics_weekday(ics_days_from_civil(2026, 10, 14)) == 2);   /* Wednesday */
    int y, m, d;
    ics_civil_from_days(ics_days_from_civil(2024, 2, 29), &y, &m, &d);
    CHECK(y == 2024 && m == 2 && d == 29);
    bool date_only, utc;
    CHECK(ics_parse_time("20261014T090000", &date_only, &utc) == at(2026, 10, 14, 9, 0) && !date_only && !utc);
    CHECK(ics_parse_time("20261014", &date_only, &utc) == at(2026, 10, 14, 0, 0) && date_only);
    CHECK(ics_parse_time("20261014T070000Z", &date_only, &utc) == at(2026, 10, 14, 7, 0) && utc);
    CHECK(ics_parse_time("20261332", &date_only, &utc) == -1 && ics_parse_time("2026", &date_only, &utc) == -1);

    ics_time_t from = at(2026, 10, 12, 0, 0), to = at(2026, 11, 30, 0, 0);
    for (int chunk = 1; chunk <= 4096; chunk *= 7)
    {
        run(CALENDAR, from, to, true, chunk);

        int i = find(at(2026, 10, 14, 9, 0), "Riunione, team prodotto");
        CHECK(i >= 0 && s_out[i].end == at(2026, 10, 14, 10, 0) && eq(s_out[i].location, "Sala 2"));
        i = find(at(2026, 10, 15, 16, 30), "Chiamata");
        CHECK(i >= 0 && s_out[i].end == at(2026, 10, 15, 17, 15));
        i = find(at(2026, 10, 16, 0, 0), "Compleanno 🎂");
        CHECK(i >= 0 && s_out[i].all_day && s_out[i].end == at(2026, 10, 17, 0, 0));

        /* Gym: Mon 12, Wed 14, (Mon 19 excluded), Wed 21 moved to 19:00, (Mon 26 cancelled), Wed 28 is
         * after UNTIL 21:59:59Z = 22:59:59 local? No: 28 Oct 8:00 local < 22:59 -> included. */
        CHECK(find(at(2026, 10, 12, 8, 0), "Palestra") >= 0 && find(at(2026, 10, 14, 8, 0), "Palestra") >= 0);
        CHECK(find(at(2026, 10, 19, 8, 0), "Palestra") < 0);
        CHECK(find(at(2026, 10, 21, 8, 0), "Palestra") < 0 && find(at(2026, 10, 21, 19, 0), "Palestra") >= 0);
        CHECK(find(at(2026, 10, 26, 8, 0), "Palestra") < 0);
        CHECK(find(at(2026, 10, 28, 8, 0), "Palestra") >= 0 && find(at(2026, 11, 2, 8, 0), "Palestra") < 0);
        CHECK(count_title("Palestra") == 4);

        /* Club: 2nd Tuesday of Sep (8), Oct (13), Nov (10); only Oct and Nov in the window. */
        CHECK(find(at(2026, 10, 13, 18, 0), "Club") >= 0 && find(at(2026, 11, 10, 18, 0), "Club") >= 0);
        CHECK(count_title("Club") == 2);

        /* Rent on the last day: 31 Oct (Saturday, all day). Not 30 Nov: the window ends at its start. */
        CHECK(find(at(2026, 10, 31, 0, 0), "Affitto") >= 0 && count_title("Affitto") == 1);

        /* No 29 February in the window. */
        CHECK(count_title("Bisestile") == 0);

        /* Pill: 8,10,12,14,16,18 Oct (COUNT 6): in the window 12,14,16,18. */
        CHECK(count_title("Pillola") == 4 && find(at(2026, 10, 18, 7, 0), "Pillola") >= 0);
        CHECK(find(at(2026, 10, 20, 7, 0), "Pillola") < 0);

        CHECK(count_title("Annullato") == 0 && count_title("Vecchio") == 0);
    }

    /* Leap-day yearly event in a window holding 29 Feb 2028. */
    run(CALENDAR, at(2028, 2, 1, 0, 0), at(2028, 3, 2, 0, 0), true, 64);
    CHECK(find(at(2028, 2, 29, 0, 0), "Bisestile") >= 0);
    /* Rent in February 2028: the 29th. */
    CHECK(find(at(2028, 2, 29, 0, 0), "Affitto") >= 0);

    /* Long UTF-8 titles are cut on a character boundary. */
    static const char LONG[] =
        "BEGIN:VEVENT\nDTSTART:20261014T090000\nSUMMARY:"
        "èèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèèè\nEND:VEVENT\n";
    run(LONG, from, to, false, 13);
    CHECK(s_count == 1);
    int n = length(s_out[0].title);
    CHECK(n > 60 && n < ICS_TEXT && n % 2 == 0);
    return 0;
}

#ifdef GIORNATA_EMULATOR_ENTRY
__attribute__((naked)) void _start(void)
{
    /* Full-size ebreak: the runner looks for the 4-byte instruction. */
    __asm__ volatile(".option push\n .option norvc\n call giornata_tests\n ebreak\n .option pop\n");
}
#else
#include <stdio.h>
int main(void)
{
    int line = giornata_tests();
    if (line) printf("FAIL at line %d\n", line);
    return line;
}
#endif
