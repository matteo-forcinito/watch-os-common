#ifndef GIORNATA_ICS_H
#define GIORNATA_ICS_H

/*
 * iCalendar (RFC 5545) reader for Giornata: pure C, no libc, no ESP-IDF
 * (tested in an emulator like the other apps' maths).
 *
 * Times are "civil seconds": the local wall-clock date and time counted as if
 * it were UTC (days_from_civil * 86400 + h*3600 + m*60 + s). Recurrences are
 * expanded in civil time, so a weekly 9:00 meeting stays at 9:00 across the
 * daylight-saving change. Times written in UTC ("...Z") are turned into local
 * civil time by the caller's function (it knows the time zone).
 *
 * Use:
 *   ics_init(), then ics_feed_text() with chunks of the file (lines may be
 *   split anywhere), then ics_finish().
 *   Single events come out of `emit` at once (if inside the window).
 *   Recurring events go to `defer`: a moved or cancelled instance
 *   (RECURRENCE-ID) may come later in the file, so the caller keeps them and
 *   calls ics_expand() for each after ics_finish(). Without `defer` they are
 *   expanded at once (fine when there are no moved instances).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ICS_TEXT       72   /* title / location kept (UTF-8, cut at a character boundary) */
#define ICS_UID        64
#define ICS_EXCEPTIONS 24   /* EXDATE per event */
#define ICS_BYDAY      7
#define ICS_LINE       1024
#define ICS_OVERRIDES  128  /* RECURRENCE-ID instances remembered per file */

typedef int64_t ics_time_t;           /* civil seconds (see above) */

enum { ICS_NONE, ICS_DAILY, ICS_WEEKLY, ICS_MONTHLY, ICS_YEARLY };

typedef struct
{
    char uid[ICS_UID];
    char title[ICS_TEXT];
    char location[ICS_TEXT];
    ics_time_t start;
    ics_time_t end;
    int64_t duration;               /* seconds, -1 = none */
    bool all_day;
    bool cancelled;
    bool has_start;
    bool has_end;
    ics_time_t recurrence_id;       /* != 0: this VEVENT replaces one instance */
    /* RRULE */
    int freq;                       /* ICS_NONE .. ICS_YEARLY */
    int interval;
    int count;                      /* 0 = none */
    ics_time_t until;               /* 0 = none */
    int byday_count;
    int8_t byday[ICS_BYDAY];        /* 0 = Monday .. 6 = Sunday */
    int8_t byday_nth[ICS_BYDAY];    /* 0 = every, 2 = second, -1 = last */
    int8_t bymonthday;              /* 0 = none, -1 = last day */
    int exception_count;
    ics_time_t exceptions[ICS_EXCEPTIONS];
} ics_event_t;

typedef struct
{
    ics_time_t start;
    ics_time_t end;
    bool all_day;
    char title[ICS_TEXT];
    char location[ICS_TEXT];
} ics_occurrence_t;

/* UTC seconds -> local civil seconds. */
typedef ics_time_t (*ics_to_local_fn)(int64_t utc);
/* One occurrence; return false to stop reading. */
typedef bool (*ics_emit_fn)(const ics_occurrence_t *occurrence, void *ctx);
/* A recurring event to expand after the whole file (copy it). */
typedef void (*ics_defer_fn)(const ics_event_t *event, void *ctx);

typedef struct
{
    /* inputs */
    ics_time_t window_start;
    ics_time_t window_end;
    ics_to_local_fn to_local;
    ics_emit_fn emit;
    ics_defer_fn defer;
    void *ctx;

    /* state */
    char line[ICS_LINE];      /* the logical line being unfolded */
    int used;
    bool line_overflow;
    char raw[ICS_LINE];       /* the physical line being read */
    int raw_used;
    bool raw_overflow;
    bool have_line;           /* line holds a logical line waiting for continuations */
    bool in_event;
    int depth;                /* nested components inside VEVENT (VALARM) */
    bool stopped;
    ics_event_t ev;

    /* instances replaced by a RECURRENCE-ID event: hash of uid + time */
    uint32_t overrides[ICS_OVERRIDES];
    int override_count;

    /* statistics */
    int events;
    int occurrences;
} ics_reader_t;

void ics_init(ics_reader_t *r, ics_time_t window_start, ics_time_t window_end, ics_to_local_fn to_local,
              ics_emit_fn emit, ics_defer_fn defer, void *ctx);
/* Any chunk of the file. */
void ics_feed_text(ics_reader_t *r, const char *text, size_t length);
/* End of file. */
void ics_finish(ics_reader_t *r);
/* Occurrences of a recurring event inside the window (after ics_finish for deferred ones). */
void ics_expand(ics_reader_t *r, const ics_event_t *event);

/* Helpers, exported for tests and for the app. */
int64_t ics_days_from_civil(int year, int month, int day);
void ics_civil_from_days(int64_t days, int *year, int *month, int *day);
int ics_weekday(int64_t days);   /* 0 = Monday */
/* "20261014T090000" / "20261014" / "20261014T070000Z": civil seconds; flags set; -1 if bad. */
ics_time_t ics_parse_time(const char *text, bool *date_only, bool *utc);

#endif
