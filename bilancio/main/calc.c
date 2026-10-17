/* Bilancio's pure logic: see calc.h. No libc: runs as is in the emulator tests. */
#include "calc.h"

/* ------------------------------------------------------------- calendar */

bl_day_t bl_days_from_civil(int year, int month, int day)
{
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    int yoe = year - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void bl_civil(bl_day_t days, int *year, int *month, int *day)
{
    int z = days + 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int m = mp < 10 ? mp + 3 : mp - 9;
    *day = doy - (153 * mp + 2) / 5 + 1;
    *month = m;
    *year = yoe + era * 400 + (m <= 2);
}

int bl_weekday(bl_day_t days)
{
    int w = (days + 3) % 7;          /* 1970-01-01 was a Thursday */
    return w < 0 ? w + 7 : w;
}

int bl_days_in_month(int year, int month)
{
    static const unsigned char days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
    {
        return 29;
    }
    return days[(month - 1) % 12];
}

int bl_month_index(int year, int month)
{
    return year * 12 + month - 1;
}

int bl_month_of(bl_day_t days)
{
    int y, m, d;
    bl_civil(days, &y, &m, &d);
    return bl_month_index(y, m);
}

bl_day_t bl_month_start(int month_index)
{
    return bl_days_from_civil(month_index / 12, month_index % 12 + 1, 1);
}

static int digits(const char *text, int count, bool *ok)
{
    int value = 0;
    for (int i = 0; i < count; i++)
    {
        if (text[i] < '0' || text[i] > '9')
        {
            *ok = false;
            return 0;
        }
        value = value * 10 + text[i] - '0';
    }
    return value;
}

int bl_parse_month(const char *text)
{
    if (text == 0)
    {
        return 0;
    }
    bool ok = true;
    int year = digits(text, 4, &ok);
    if (!ok || text[4] != '-')
    {
        return 0;
    }
    int month = digits(text + 5, 2, &ok);
    if (!ok || (text[7] != '\0' && text[7] != '-') || year < 1970 || year > 2200 || month < 1 || month > 12)
    {
        return 0;
    }
    return bl_month_index(year, month);
}

bl_day_t bl_parse_date(const char *text)
{
    int month = bl_parse_month(text);
    if (month == 0 || text[7] != '-')
    {
        return BL_NONE;
    }
    bool ok = true;
    int day = digits(text + 8, 2, &ok);
    int y = month / 12, m = month % 12 + 1;
    if (!ok || text[10] != '\0' || day < 1 || day > bl_days_in_month(y, m))
    {
        return BL_NONE;
    }
    return bl_days_from_civil(y, m, day);
}

static void put(char *out, int value, int count)
{
    for (int i = count - 1; i >= 0; i--)
    {
        out[i] = (char)('0' + value % 10);
        value /= 10;
    }
}

void bl_format_month(int month_index, char *out)
{
    put(out, month_index / 12, 4);
    out[4] = '-';
    put(out + 5, month_index % 12 + 1, 2);
    out[7] = '\0';
}

void bl_format_date(bl_day_t days, char *out)
{
    int y, m, d;
    bl_civil(days, &y, &m, &d);
    bl_format_month(bl_month_index(y, m), out);
    out[7] = '-';
    put(out + 8, d, 2);
    out[10] = '\0';
}

bl_cents_t bl_cents(double euros)
{
    double cents = euros * 100.0;
    return (bl_cents_t)(cents < 0 ? cents - 0.5 : cents + 0.5);
}

/* ---------------------------------------------------------------- rules */

bl_day_t bl_shift(bl_day_t days, int shift)
{
    int w = bl_weekday(days);
    if (w < 5 || shift == BL_SHIFT_EXACT)
    {
        return days;
    }
    if (shift == BL_SHIFT_BEFORE)
    {
        return days - (w - 4);        /* Saturday -1, Sunday -2: Friday */
    }
    return days + (7 - w);            /* Saturday +2, Sunday +1: Monday */
}

static int every_of(const bl_rule_t *rule)
{
    return rule->every == 0 ? 1 : rule->every;
}

/* Beyond a year `first` only anchors the cycle: the money is set aside before it. */
static bool active(const bl_rule_t *rule, int month_index)
{
    bool started = rule->first == 0 || every_of(rule) > 12 || month_index >= rule->first;
    return started && (rule->last == 0 || month_index <= rule->last);
}

bool bl_rule_in_month(const bl_rule_t *rule, int month_index)
{
    if (!active(rule, month_index))
    {
        return false;
    }
    int every = every_of(rule);
    /* Up to a year the month says where the cycle falls; beyond (car inspection every
     * 2 years, licence every 10) the first month does. */
    int anchor = every > 12 && rule->first != 0 ? rule->first : rule->month >= 1 && rule->month <= 12 ? rule->month - 1 : 0;
    int phase = (month_index - anchor) % every;
    return phase == 0;
}

bl_day_t bl_occurrence(const bl_rule_t *rule, int month_index)
{
    if (!bl_rule_in_month(rule, month_index))
    {
        return BL_NONE;
    }
    int y = month_index / 12, m = month_index % 12 + 1;
    int last = bl_days_in_month(y, m);
    int day = rule->day < 1 ? 1 : rule->day > last ? last : rule->day;
    return bl_shift(bl_days_from_civil(y, m, day), rule->shift);
}

bl_day_t bl_next(const bl_rule_t *rule, bl_day_t after)
{
    int start = bl_month_of(after) - 1;
    for (int mi = start; mi <= start + every_of(rule) + 2; mi++)
    {
        if (rule->last != 0 && mi > rule->last)
        {
            break;
        }
        bl_day_t date = bl_occurrence(rule, mi);
        if (date != BL_NONE && date > after)
        {
            return date;
        }
    }
    return BL_NONE;
}

bl_day_t bl_previous(const bl_rule_t *rule, bl_day_t day)
{
    int start = bl_month_of(day) + 1;
    for (int mi = start; mi >= start - every_of(rule) - 2; mi--)
    {
        if (rule->first != 0 && mi < rule->first)
        {
            break;
        }
        bl_day_t date = bl_occurrence(rule, mi);
        if (date != BL_NONE && date <= day)
        {
            return date;
        }
    }
    return BL_NONE;
}

void bl_each_date(const bl_rule_t *rules, int count, bl_day_t from, bl_day_t to, bl_visit_t visit, void *ctx)
{
    if (to <= from)
    {
        return;
    }
    int first = bl_month_of(from) - 1, last = bl_month_of(to - 1) + 1;
    for (int i = 0; i < count; i++)
    {
        for (int mi = first; mi <= last; mi++)
        {
            bl_day_t date = bl_occurrence(&rules[i], mi);
            if (date != BL_NONE && date >= from && date < to)
            {
                visit(&rules[i], date, ctx);
            }
        }
    }
}

bool bl_paid(const bl_book_t *book, const bl_rule_t *rule, bl_day_t date)
{
    for (int i = 0; i < book->tx_count; i++)
    {
        const bl_tx_t *t = &book->tx[i];
        if (t->rule == rule->id && t->date >= date - BL_LINK_DAYS && t->date <= date + BL_LINK_DAYS)
        {
            return true;
        }
    }
    return false;
}

int bl_main_income(const bl_rule_t *rules, int count)
{
    int best = -1;
    for (int i = 0; i < count; i++)
    {
        const bl_rule_t *r = &rules[i];
        if (r->kind != BL_INCOME || r->amount <= 0)
        {
            continue;
        }
        if (best < 0)
        {
            best = i;
            continue;
        }
        bool monthly = every_of(r) == 1, best_monthly = every_of(&rules[best]) == 1;
        if (monthly != best_monthly ? monthly : r->amount > rules[best].amount)
        {
            best = i;
        }
    }
    return best;
}

bl_day_t bl_next_payday(const bl_book_t *book, bl_day_t today)
{
    int main = bl_main_income(book->rules, book->rule_count);
    bl_day_t date = main >= 0 ? bl_next(&book->rules[main], today) : BL_NONE;
    return date != BL_NONE ? date : bl_month_start(bl_month_of(today) + 1);
}

/* ------------------------------------------------------------- analysis */

typedef struct
{
    const bl_book_t *book;
    int kind;               /* -1: both, signed (income +, expense -) */
    bl_day_t after;         /* only dates after this day */
    bl_day_t expense_before;/* expenses only before this day */
    bl_cents_t total;
} sum_t;

static void add_date(const bl_rule_t *rule, bl_day_t date, void *ctx)
{
    sum_t *s = ctx;
    if (date <= s->after || (s->kind >= 0 && rule->kind != s->kind))
    {
        return;
    }
    if (rule->kind == BL_EXPENSE && date >= s->expense_before)
    {
        return;
    }
    if (bl_paid(s->book, rule, date))
    {
        return;
    }
    s->total += s->kind >= 0 || rule->kind == BL_INCOME ? rule->amount : -rule->amount;
}

static bool counted(const bl_book_t *book, const bl_tx_t *t)
{
    return book->balance_date == BL_NONE || (t->id > book->balance_after && t->date >= book->balance_date);
}

/*
 * Without a balance entered, the book starts on its first day: the earliest of
 * the first movement, the last payday and the first of the month. The balance
 * is then income minus expenses since then: before the first salary entered
 * arrives the balance is negative, on purpose (that money is not there yet).
 */
static bl_book_t effective(const bl_book_t *book, bl_day_t today)
{
    bl_book_t e = *book;
    if (book->balance_date != BL_NONE)
    {
        return e;
    }
    bl_day_t start = bl_month_start(bl_month_of(today));
    int main = bl_main_income(book->rules, book->rule_count);
    bl_cents_t opening = 0;
    bl_day_t payday = BL_NONE;
    if (main >= 0)
    {
        /* The last salary already paid counts even before the rule's first month. */
        bl_rule_t since_always = book->rules[main];
        since_always.first = 0;
        payday = bl_previous(&since_always, today);
        if (payday != BL_NONE && bl_previous(&book->rules[main], today) != payday)
        {
            opening = book->rules[main].amount;
        }
    }
    if (payday != BL_NONE && payday < start)
    {
        start = payday;
    }
    for (int i = 0; i < book->tx_count; i++)
    {
        if (book->tx[i].date < start)
        {
            start = book->tx[i].date;
        }
    }
    e.balance = opening;
    e.balance_after = 0;
    e.balance_date = start - 1;
    return e;
}

bl_cents_t bl_balance_today(const bl_book_t *original, bl_day_t today)
{
    bl_book_t e = effective(original, today);
    const bl_book_t *book = &e;
    bl_cents_t balance = book->balance;
    for (int i = 0; i < book->tx_count; i++)
    {
        const bl_tx_t *t = &book->tx[i];
        if (counted(book, t) && t->date <= today)
        {
            balance += t->amount;
        }
    }
    sum_t s = {.book = book, .kind = -1, .after = book->balance_date, .expense_before = today};
    bl_each_date(book->rules, book->rule_count, book->balance_date + 1, today + 1, add_date, &s);
    return balance + s.total;
}

bl_cents_t bl_due_between(const bl_book_t *book, int kind, bl_day_t from, bl_day_t to)
{
    sum_t s = {.book = book, .kind = kind, .after = book->balance_date, .expense_before = to};
    bl_each_date(book->rules, book->rule_count, from, to, add_date, &s);
    return s.total;
}

static bool variable(const bl_tx_t *t, int category)
{
    return t->amount < 0 && t->rule == 0 && (category == BL_ANY_CATEGORY || t->category == category);
}

bl_cents_t bl_spent(const bl_book_t *book, int category, bl_day_t from, bl_day_t to)
{
    bl_cents_t total = 0;
    for (int i = 0; i < book->tx_count; i++)
    {
        const bl_tx_t *t = &book->tx[i];
        if (variable(t, category) && t->date >= from && t->date < to)
        {
            total -= t->amount;
        }
    }
    return total;
}

static bl_day_t first_movement(const bl_book_t *book)
{
    bl_day_t first = BL_NONE;
    for (int i = 0; i < book->tx_count; i++)
    {
        if (first == BL_NONE || book->tx[i].date < first)
        {
            first = book->tx[i].date;
        }
    }
    return first;
}

bl_cents_t bl_pace(const bl_book_t *book, bl_day_t today)
{
    bl_day_t first = first_movement(book);
    if (first == BL_NONE || first >= today)
    {
        return 0;
    }
    bl_day_t from = first > today - BL_PACE_DAYS ? first : today - BL_PACE_DAYS;
    int span = today - from;
    if (span < 7)
    {
        span = 7;               /* two days of data are not a habit */
    }
    return bl_spent(book, BL_ANY_CATEGORY, from, today) / span;
}

void bl_summary(const bl_book_t *original, bl_day_t today, bl_summary_t *out)
{
    bl_book_t e = effective(original, today);
    const bl_book_t *book = &e;
    /* Something to count: a balance entered, or income, costs or movements to add up. */
    out->has_balance = original->balance_date != BL_NONE || original->rule_count > 0 || original->tx_count > 0;
    out->balance = bl_balance_today(book, today);
    out->main_income = bl_main_income(book->rules, book->rule_count);
    out->payday = bl_next_payday(book, today);
    out->period_start = out->main_income >= 0 ? bl_previous(&book->rules[out->main_income], today) : BL_NONE;
    if (out->period_start == BL_NONE)
    {
        out->period_start = bl_month_start(bl_month_of(today));
    }
    out->days = out->payday - today < 1 ? 1 : out->payday - today;
    out->due = bl_due_between(book, BL_EXPENSE, today, out->payday);
    out->income = bl_due_between(book, BL_INCOME, today + 1, out->payday);
    out->spent_today = 0;
    for (int i = 0; i < book->tx_count; i++)
    {
        const bl_tx_t *t = &book->tx[i];
        if (t->date > today && t->date < out->payday)
        {
            if (t->amount < 0)          /* planned */
            {
                out->due -= t->amount;
            }
            else
            {
                out->income += t->amount;
            }
        }
        else if (t->date == today && variable(t, BL_ANY_CATEGORY) && counted(book, t))
        {
            out->spent_today -= t->amount;
        }
    }
    out->free = out->balance - out->due + out->income;
    bl_cents_t start_of_day = out->free + out->spent_today;
    out->daily = start_of_day > 0 ? start_of_day / out->days : 0;
    out->today_left = out->daily - out->spent_today;
    out->pace = bl_pace(book, today);
    bl_cents_t rest_of_today = out->pace > out->spent_today ? out->pace - out->spent_today : 0;
    out->forecast = out->free - rest_of_today - out->pace * (out->days - 1);
    out->period_spent = bl_spent(book, BL_ANY_CATEGORY, out->period_start, today + 1);
    if (out->free < 0 || out->forecast < 0)
    {
        out->state = BL_SHORT;
    }
    else if (out->today_left < 0 || out->pace > out->daily)
    {
        out->state = BL_FAST;
    }
    else
    {
        out->state = BL_OK;
    }
}

bl_cents_t bl_month_total(const bl_rule_t *rules, int count, int kind, int month_index)
{
    bl_cents_t total = 0;
    for (int i = 0; i < count; i++)
    {
        if (rules[i].kind == kind && bl_rule_in_month(&rules[i], month_index))
        {
            total += rules[i].amount;
        }
    }
    return total;
}

bl_cents_t bl_monthly_share(const bl_rule_t *rules, int count, int kind, int month_index)
{
    bl_cents_t total = 0;
    for (int i = 0; i < count; i++)
    {
        if (rules[i].kind == kind && active(&rules[i], month_index))
        {
            total += rules[i].amount / every_of(&rules[i]);
        }
    }
    return total;
}

int bl_fixed_ratio(const bl_rule_t *rules, int count, int month_index)
{
    bl_cents_t income = bl_monthly_share(rules, count, BL_INCOME, month_index);
    if (income <= 0)
    {
        return -1;
    }
    return (int)(bl_monthly_share(rules, count, BL_EXPENSE, month_index) * 100 / income);
}

bl_cents_t bl_yearly_cost(const bl_rule_t *rules, int count, int category, int month_index)
{
    bl_cents_t total = 0;
    for (int i = 0; i < count; i++)
    {
        const bl_rule_t *r = &rules[i];
        if (r->kind == BL_EXPENSE && active(r, month_index) && (category == BL_ANY_CATEGORY || r->category == category))
        {
            total += r->amount * 12 / every_of(r);
        }
    }
    return total;
}

bl_cents_t bl_set_aside(const bl_rule_t *rules, int count, int month_index)
{
    bl_cents_t total = 0;
    for (int i = 0; i < count; i++)
    {
        const bl_rule_t *r = &rules[i];
        if (r->kind == BL_EXPENSE && every_of(r) > 1 && active(r, month_index))
        {
            total += r->amount / every_of(r);
        }
    }
    return total;
}

bl_cents_t bl_rule_progress(const bl_rule_t *rule, bl_day_t today, int *done, int *left)
{
    *done = -1;
    *left = -1;
    if (rule->last == 0)
    {
        return 0;
    }
    int now = bl_month_of(today);
    int from = rule->first != 0 ? rule->first : now - 1;
    int past = 0, future = 0;
    for (int mi = from; mi <= rule->last; mi++)
    {
        bl_day_t date = bl_occurrence(rule, mi);
        if (date == BL_NONE)
        {
            continue;
        }
        if (date <= today)
        {
            past++;
        }
        else
        {
            future++;
        }
    }
    if (rule->first != 0)
    {
        *done = past;
    }
    *left = future;
    return rule->amount * future;
}

int bl_category_trend(const bl_book_t *book, int category, bl_day_t today, bl_cents_t *now, bl_cents_t *average)
{
    int y, m, d;
    bl_civil(today, &y, &m, &d);
    int month = bl_month_index(y, m);
    *now = bl_spent(book, category, bl_month_start(month), today + 1);
    *average = 0;
    bl_day_t first = first_movement(book);
    bl_cents_t sum = 0;
    int months = 0;
    for (int k = 1; k <= 3; k++)
    {
        int mi = month - k;
        bl_day_t start = bl_month_start(mi);
        int length = bl_days_in_month(mi / 12, mi % 12 + 1);
        bl_day_t end = start + (d < length ? d : length);
        if (first == BL_NONE || first > start)
        {
            continue;               /* not tracked yet in that window */
        }
        sum += bl_spent(book, category, start, end);
        months++;
    }
    if (months == 0)
    {
        return -1000;
    }
    *average = sum / months;
    if (*average == 0)
    {
        return *now > 0 ? 999 : 0;
    }
    bl_cents_t pct = (*now - *average) * 100 / *average;
    return pct > 999 ? 999 : (int)pct;
}
