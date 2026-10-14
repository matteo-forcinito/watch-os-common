/* iCalendar reader (see ics.h). Freestanding: no libc calls. */
#include "ics.h"

#define MAX_ITERATIONS 40000
#define MAX_PER_PERIOD 31

/* ------------------------------------------------------------ strings */

static bool starts(const char *text, const char *prefix)
{
    while (*prefix != '\0')
    {
        if (*text++ != *prefix++)
        {
            return false;
        }
    }
    return true;
}

static bool same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}

static void clear(void *p, size_t n)
{
    unsigned char *c = p;
    while (n-- > 0)
    {
        *c++ = 0;
    }
}

/* Copy an iCalendar TEXT value, unescaped, cut at a UTF-8 character boundary. */
static void copy_text(char *out, size_t size, const char *in)
{
    size_t n = 0;
    while (*in != '\0' && n < size - 1)
    {
        char c = *in++;
        if (c == '\\' && *in != '\0')
        {
            char e = *in++;
            c = (e == 'n' || e == 'N') ? ' ' : e;
        }
        out[n++] = c;
    }
    /* Do not leave half a multi-byte character at the end. */
    while (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0x80)
    {
        size_t start = n - 1;
        while (start > 0 && ((unsigned char)out[start] & 0xC0) == 0x80)
        {
            start--;
        }
        unsigned char lead = (unsigned char)out[start];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (n - start >= need)
        {
            break;
        }
        n = start;
    }
    if (n > 0 && (unsigned char)out[n - 1] >= 0xC0)
    {
        n--;
    }
    out[n] = '\0';
}

/* --------------------------------------------------------------- dates */

int64_t ics_days_from_civil(int year, int month, int day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yoe = year - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void ics_civil_from_days(int64_t z, int *year, int *month, int *day)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    const int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = (int)(y + (m <= 2));
    *month = m;
    *day = d;
}

int ics_weekday(int64_t days)
{
    /* 1970-01-01 was a Thursday (3 with Monday = 0). */
    int64_t w = (days + 3) % 7;
    return (int)(w < 0 ? w + 7 : w);
}

static int days_in_month(int year, int month)
{
    static const int8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : days[month - 1];
}

static int number(const char *t, int count, bool *ok)
{
    int value = 0;
    for (int i = 0; i < count; i++)
    {
        if (t[i] < '0' || t[i] > '9')
        {
            *ok = false;
            return 0;
        }
        value = value * 10 + (t[i] - '0');
    }
    return value;
}

ics_time_t ics_parse_time(const char *t, bool *date_only, bool *utc)
{
    bool ok = true;
    *date_only = false;
    *utc = false;
    for (int i = 0; i < 8; i++)
    {
        if (t[i] == '\0')
        {
            return -1;
        }
    }
    int year = number(t, 4, &ok), month = number(t + 4, 2, &ok), day = number(t + 6, 2, &ok);
    if (!ok || month < 1 || month > 12 || day < 1 || day > days_in_month(year, month))
    {
        return -1;
    }
    int64_t days = ics_days_from_civil(year, month, day);
    if (t[8] != 'T')
    {
        *date_only = true;
        return days * 86400;
    }
    for (int i = 9; i < 15; i++)
    {
        if (t[i] == '\0')
        {
            return -1;
        }
    }
    int hour = number(t + 9, 2, &ok), minute = number(t + 11, 2, &ok), second = number(t + 13, 2, &ok);
    if (!ok || hour > 23 || minute > 59 || second > 60)
    {
        return -1;
    }
    *utc = t[15] == 'Z';
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

/* "PT1H30M", "P1D", "P2W", "-PT15M" -> seconds, -1 if bad. */
static int64_t parse_duration(const char *t)
{
    bool negative = false;
    if (*t == '+' || *t == '-')
    {
        negative = *t == '-';
        t++;
    }
    if (*t++ != 'P')
    {
        return -1;
    }
    int64_t total = 0, value = 0;
    bool digits = false;
    for (; *t != '\0'; t++)
    {
        if (*t >= '0' && *t <= '9')
        {
            value = value * 10 + (*t - '0');
            digits = true;
            continue;
        }
        int64_t unit = *t == 'W' ? 604800 : *t == 'D' ? 86400 : *t == 'H' ? 3600 : *t == 'M' ? 60 : *t == 'S' ? 1 : 0;
        if (*t == 'T')
        {
            continue;
        }
        if (unit == 0 || !digits)
        {
            return -1;
        }
        total += value * unit;
        value = 0;
        digits = false;
    }
    return negative ? -total : total;
}

/* ------------------------------------------------------------ overrides */

static uint32_t instance_hash(const char *uid, ics_time_t when)
{
    uint32_t h = 2166136261u;
    for (; *uid != '\0'; uid++)
    {
        h = (h ^ (uint8_t)*uid) * 16777619u;
    }
    for (int i = 0; i < 8; i++)
    {
        h = (h ^ (uint8_t)(when >> (i * 8))) * 16777619u;
    }
    return h | 1;   /* never 0 */
}

static bool overridden(const ics_reader_t *r, const char *uid, ics_time_t when)
{
    uint32_t h = instance_hash(uid, when);
    for (int i = 0; i < r->override_count; i++)
    {
        if (r->overrides[i] == h)
        {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------- reading */

/* Value of a time property, in civil seconds; date_only reported. -1 if bad. */
static ics_time_t time_value(ics_reader_t *r, const char *params, const char *value, bool *date_only)
{
    bool utc;
    ics_time_t t = ics_parse_time(value, date_only, &utc);
    (void)params;   /* TZID: the watch's own zone is assumed (calendars of one country) */
    if (t >= 0 && utc && r->to_local != 0)
    {
        t = r->to_local(t);   /* civil seconds of UTC == UTC seconds */
    }
    return t;
}

static void parse_rrule(ics_event_t *ev, char *value, ics_reader_t *r)
{
    ev->interval = 1;
    char *part = value;
    while (part != 0 && *part != '\0')
    {
        char *next = part;
        while (*next != '\0' && *next != ';')
        {
            next++;
        }
        bool more = *next == ';';
        *next = '\0';
        if (starts(part, "FREQ="))
        {
            const char *f = part + 5;
            ev->freq = same(f, "DAILY") ? ICS_DAILY : same(f, "WEEKLY") ? ICS_WEEKLY :
                       same(f, "MONTHLY") ? ICS_MONTHLY : same(f, "YEARLY") ? ICS_YEARLY : ICS_NONE;
        }
        else if (starts(part, "INTERVAL="))
        {
            bool ok = true;
            int n = 0;
            for (const char *c = part + 9; *c != '\0'; c++)
            {
                n = n * 10 + number(c, 1, &ok);
            }
            ev->interval = ok && n > 0 && n < 1000 ? n : 1;
        }
        else if (starts(part, "COUNT="))
        {
            bool ok = true;
            int n = 0;
            for (const char *c = part + 6; *c != '\0'; c++)
            {
                n = n * 10 + number(c, 1, &ok);
            }
            ev->count = ok && n > 0 ? n : 0;
        }
        else if (starts(part, "UNTIL="))
        {
            bool date_only;
            ics_time_t until = time_value(r, "", part + 6, &date_only);
            ev->until = until < 0 ? 0 : date_only ? until + 86399 : until;
        }
        else if (starts(part, "BYMONTHDAY="))
        {
            const char *c = part + 11;
            bool negative = *c == '-', ok = true;
            c += negative || *c == '+';
            int n = 0;
            for (; *c != '\0' && *c != ','; c++)
            {
                n = n * 10 + number(c, 1, &ok);
            }
            ev->bymonthday = (int8_t)(ok && n > 0 && n <= 31 ? (negative ? -n : n) : 0);
        }
        else if (starts(part, "BYDAY="))
        {
            static const char *const names[] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
            const char *c = part + 6;
            while (*c != '\0' && ev->byday_count < ICS_BYDAY)
            {
                bool negative = false;
                int nth = 0;
                if (*c == '-' || *c == '+')
                {
                    negative = *c == '-';
                    c++;
                }
                while (*c >= '0' && *c <= '9')
                {
                    nth = nth * 10 + (*c - '0');
                    c++;
                }
                for (int d = 0; d < 7; d++)
                {
                    if (c[0] == names[d][0] && c[1] == names[d][1])
                    {
                        ev->byday[ev->byday_count] = (int8_t)d;
                        ev->byday_nth[ev->byday_count] = (int8_t)(negative ? -nth : nth);
                        ev->byday_count++;
                        break;
                    }
                }
                while (*c != '\0' && *c != ',')
                {
                    c++;
                }
                if (*c == ',')
                {
                    c++;
                }
            }
        }
        part = more ? next + 1 : 0;
    }
}

static void emit_occurrence(ics_reader_t *r, const ics_event_t *ev, ics_time_t start, ics_time_t end)
{
    if (r->stopped || end <= r->window_start || start >= r->window_end)
    {
        return;
    }
    ics_occurrence_t o;
    o.start = start;
    o.end = end;
    o.all_day = ev->all_day;
    copy_text(o.title, sizeof(o.title), ev->title);
    copy_text(o.location, sizeof(o.location), ev->location);
    r->occurrences++;
    if (r->emit != 0 && !r->emit(&o, r->ctx))
    {
        r->stopped = true;
    }
}

static void end_event(ics_reader_t *r)
{
    ics_event_t *ev = &r->ev;
    if (!ev->has_start)
    {
        return;
    }
    if (!ev->has_end)
    {
        ev->end = ev->duration >= 0 ? ev->start + ev->duration : ev->all_day ? ev->start + 86400 : ev->start;
    }
    if (ev->end < ev->start)
    {
        ev->end = ev->start;
    }
    r->events++;
    if (ev->recurrence_id != 0)
    {
        /* A moved or cancelled instance of a recurring event. */
        if (r->override_count < ICS_OVERRIDES)
        {
            r->overrides[r->override_count++] = instance_hash(ev->uid, ev->recurrence_id);
        }
        if (!ev->cancelled)
        {
            emit_occurrence(r, ev, ev->start, ev->end);
        }
        return;
    }
    if (ev->cancelled)
    {
        return;
    }
    if (ev->freq != ICS_NONE)
    {
        if (r->defer != 0)
        {
            r->defer(ev, r->ctx);
        }
        else
        {
            ics_expand(r, ev);
        }
        return;
    }
    emit_occurrence(r, ev, ev->start, ev->end);
}

/* One unfolded content line: NAME;PARAMS:VALUE */
static void content_line(ics_reader_t *r, char *line)
{
    if (starts(line, "BEGIN:"))
    {
        if (same(line + 6, "VEVENT") && !r->in_event)
        {
            r->in_event = true;
            r->depth = 0;
            clear(&r->ev, sizeof(r->ev));
            r->ev.duration = -1;
        }
        else if (r->in_event)
        {
            r->depth++;   /* VALARM inside the event: its properties are not the event's */
        }
        return;
    }
    if (starts(line, "END:"))
    {
        if (r->in_event && r->depth > 0)
        {
            r->depth--;
        }
        else if (r->in_event && same(line + 4, "VEVENT"))
        {
            r->in_event = false;
            end_event(r);
        }
        return;
    }
    if (!r->in_event || r->depth > 0)
    {
        return;
    }
    /* Split name / params / value; ':' inside quoted parameters is not the separator. */
    char *name = line, *params = 0, *value = 0;
    bool quoted = false;
    for (char *c = line; *c != '\0'; c++)
    {
        if (*c == '"')
        {
            quoted = !quoted;
        }
        else if (*c == ';' && params == 0 && !quoted)
        {
            *c = '\0';
            params = c + 1;
        }
        else if (*c == ':' && !quoted)
        {
            *c = '\0';
            value = c + 1;
            break;
        }
    }
    if (value == 0)
    {
        return;
    }
    if (params == 0)
    {
        params = "";
    }
    ics_event_t *ev = &r->ev;
    bool date_only = false;
    if (same(name, "UID"))
    {
        copy_text(ev->uid, sizeof(ev->uid), value);
    }
    else if (same(name, "SUMMARY"))
    {
        copy_text(ev->title, sizeof(ev->title), value);
    }
    else if (same(name, "LOCATION"))
    {
        copy_text(ev->location, sizeof(ev->location), value);
    }
    else if (same(name, "DTSTART"))
    {
        ics_time_t t = time_value(r, params, value, &date_only);
        if (t >= 0)
        {
            ev->start = t;
            ev->all_day = date_only;
            ev->has_start = true;
        }
    }
    else if (same(name, "DTEND"))
    {
        ics_time_t t = time_value(r, params, value, &date_only);
        if (t >= 0)
        {
            ev->end = t;
            ev->has_end = true;
        }
    }
    else if (same(name, "DURATION"))
    {
        ev->duration = parse_duration(value);
    }
    else if (same(name, "STATUS"))
    {
        ev->cancelled = same(value, "CANCELLED");
    }
    else if (same(name, "RRULE"))
    {
        parse_rrule(ev, value, r);
    }
    else if (same(name, "RECURRENCE-ID"))
    {
        ics_time_t t = time_value(r, params, value, &date_only);
        ev->recurrence_id = t > 0 ? t : 0;
    }
    else if (same(name, "EXDATE"))
    {
        char *item = value;
        while (item != 0 && *item != '\0' && ev->exception_count < ICS_EXCEPTIONS)
        {
            char *next = item;
            while (*next != '\0' && *next != ',')
            {
                next++;
            }
            bool more = *next == ',';
            *next = '\0';
            ics_time_t t = time_value(r, params, item, &date_only);
            if (t >= 0)
            {
                ev->exceptions[ev->exception_count++] = t;
            }
            item = more ? next + 1 : 0;
        }
    }
}

static void flush_logical(ics_reader_t *r)
{
    if (r->have_line)
    {
        r->line[r->used] = '\0';
        if (!r->line_overflow)
        {
            content_line(r, r->line);
        }
        r->have_line = false;
        r->used = 0;
        r->line_overflow = false;
    }
}

static void physical_line(ics_reader_t *r)
{
    r->raw[r->raw_used] = '\0';
    if (r->raw_used > 0 && r->raw[r->raw_used - 1] == '\r')
    {
        r->raw[--r->raw_used] = '\0';
    }
    bool continuation = (r->raw[0] == ' ' || r->raw[0] == '\t') && r->have_line;
    if (!continuation)
    {
        flush_logical(r);
        r->have_line = true;
    }
    if (r->raw_overflow)
    {
        r->line_overflow = true;
    }
    const char *from = continuation ? r->raw + 1 : r->raw;
    for (; *from != '\0'; from++)
    {
        if (r->used < ICS_LINE - 1)
        {
            r->line[r->used++] = *from;
        }
        else
        {
            /* Long DESCRIPTIONs are common: they are not needed, so an overflow only drops that line. */
            r->line_overflow = true;
        }
    }
    r->raw_used = 0;
    r->raw_overflow = false;
}

void ics_init(ics_reader_t *r, ics_time_t window_start, ics_time_t window_end, ics_to_local_fn to_local,
              ics_emit_fn emit, ics_defer_fn defer, void *ctx)
{
    clear(r, sizeof(*r));
    r->window_start = window_start;
    r->window_end = window_end;
    r->to_local = to_local;
    r->emit = emit;
    r->defer = defer;
    r->ctx = ctx;
}

void ics_feed_text(ics_reader_t *r, const char *text, size_t length)
{
    for (size_t i = 0; i < length && !r->stopped; i++)
    {
        char c = text[i];
        if (c == '\n')
        {
            physical_line(r);
        }
        else if (r->raw_used < ICS_LINE - 1)
        {
            r->raw[r->raw_used++] = c;
        }
        else
        {
            r->raw_overflow = true;
        }
    }
}

void ics_finish(ics_reader_t *r)
{
    if (r->raw_used > 0)
    {
        physical_line(r);
    }
    flush_logical(r);
}

/* ----------------------------------------------------------- expansion */

static void sort_days(int64_t *days, int n)
{
    for (int i = 1; i < n; i++)
    {
        int64_t key = days[i];
        int j = i - 1;
        while (j >= 0 && days[j] > key)
        {
            days[j + 1] = days[j];
            j--;
        }
        days[j + 1] = key;
    }
}

/* The nth (1-based, or -1 = last) weekday of a month; -1 if it does not exist. */
static int64_t nth_weekday(int year, int month, int weekday, int nth)
{
    int64_t first = ics_days_from_civil(year, month, 1);
    int length = days_in_month(year, month);
    if (nth > 0)
    {
        int offset = (weekday - ics_weekday(first) + 7) % 7 + (nth - 1) * 7;
        return offset < length ? first + offset : -1;
    }
    int64_t last = first + length - 1;
    int offset = (ics_weekday(last) - weekday + 7) % 7 + (-nth - 1) * 7;
    return offset < length ? last - offset : -1;
}

/* Candidate days of one period (week/month/year/day), sorted. */
static int period_days(const ics_event_t *ev, int64_t day0, int64_t period, int64_t *out)
{
    int n = 0, year, month, day;
    ics_civil_from_days(day0, &year, &month, &day);
    switch (ev->freq)
    {
    case ICS_DAILY:
    {
        int64_t d = day0 + period * ev->interval;
        bool ok = ev->byday_count == 0;
        for (int i = 0; i < ev->byday_count; i++)
        {
            ok |= ev->byday[i] == ics_weekday(d);
        }
        if (ok)
        {
            out[n++] = d;
        }
        break;
    }
    case ICS_WEEKLY:
    {
        int64_t monday = day0 - ics_weekday(day0) + period * 7 * ev->interval;
        if (ev->byday_count == 0)
        {
            out[n++] = monday + ics_weekday(day0);
        }
        for (int i = 0; i < ev->byday_count; i++)
        {
            out[n++] = monday + ev->byday[i];
        }
        break;
    }
    case ICS_MONTHLY:
    {
        int64_t index = (int64_t)(month - 1) + period * ev->interval;
        int y = year + (int)(index / 12), m = (int)(index % 12) + 1;
        if (ev->byday_count > 0)
        {
            for (int i = 0; i < ev->byday_count && n < MAX_PER_PERIOD; i++)
            {
                if (ev->byday_nth[i] != 0)
                {
                    int64_t d = nth_weekday(y, m, ev->byday[i], ev->byday_nth[i]);
                    if (d >= 0)
                    {
                        out[n++] = d;
                    }
                }
                else
                {
                    for (int k = 1; k <= 5 && n < MAX_PER_PERIOD; k++)
                    {
                        int64_t d = nth_weekday(y, m, ev->byday[i], k);
                        if (d >= 0)
                        {
                            out[n++] = d;
                        }
                    }
                }
            }
        }
        else
        {
            int want = ev->bymonthday != 0 ? ev->bymonthday : day;
            int length = days_in_month(y, m);
            int d = want < 0 ? length + want + 1 : want;
            if (d >= 1 && d <= length)
            {
                out[n++] = ics_days_from_civil(y, m, d);   /* the 31st skips short months */
            }
        }
        break;
    }
    case ICS_YEARLY:
    {
        int y = year + (int)(period * ev->interval);
        if (day <= days_in_month(y, month))
        {
            out[n++] = ics_days_from_civil(y, month, day);   /* 29 February: leap years only */
        }
        break;
    }
    default:
        break;
    }
    sort_days(out, n);
    return n;
}

void ics_expand(ics_reader_t *r, const ics_event_t *ev)
{
    int64_t day0 = ev->start / 86400;
    ics_time_t time_of_day = ev->start - day0 * 86400;
    int64_t length = ev->end - ev->start;
    int64_t days[MAX_PER_PERIOD];
    int counted = 0, iterations = 0;
    for (int64_t period = 0; !r->stopped && iterations < MAX_ITERATIONS; period++)
    {
        int n = period_days(ev, day0, period, days);
        iterations++;
        if (n == 0 && ev->freq == ICS_YEARLY && period > 8 * 400)
        {
            break;
        }
        for (int i = 0; i < n; i++)
        {
            if (days[i] < day0)
            {
                continue;   /* before DTSTART inside the first period */
            }
            ics_time_t start = days[i] * 86400 + time_of_day;
            if ((ev->until != 0 && start > ev->until) || (ev->count != 0 && counted >= ev->count) ||
                start >= r->window_end)
            {
                return;
            }
            counted++;
            bool skip = false;
            for (int e = 0; e < ev->exception_count && !skip; e++)
            {
                skip = ev->all_day ? ev->exceptions[e] / 86400 == days[i] : ev->exceptions[e] == start;
            }
            if (!skip && !overridden(r, ev->uid, start))
            {
                emit_occurrence(r, ev, start, start + length);
            }
        }
    }
}
