#ifndef BILANCIO_CALC_H
#define BILANCIO_CALC_H

/*
 * Bilancio's pure logic (no ESP-IDF, no libc: tested in an emulator).
 *
 * Money is always whole cents (int64), never float. Dates are days since
 * 1970-01-01 (local calendar, no time of day); months are an index
 * year * 12 + (month - 1), 0 meaning "none".
 *
 * Rules (salary, rent, car loan, subscriptions...) generate their dates on
 * the fly: nothing in the future is stored. A rule falls every `every`
 * months on `day` (past the end of a short month: its last day), in the
 * months of `month` + k * every, between `first` and `last` (every > 12, as
 * the car inspection every 2 years: `first` + k * every). If the date is
 * a Saturday or Sunday, `shift` moves it to the Friday before or the Monday
 * after. The nominal month decides first/last, even when the shift moves
 * the date into the previous month (salary of the 1st on a Sunday -> the
 * Friday before).
 *
 * The balance. The user enters what the bank says on a day; from then on:
 *   + every movement recorded after it (higher id) and not dated before it;
 *   + every income date after that day, up to today included;
 *   - every expense date after that day and before today;
 * a rule's date never counts twice when a movement is linked to it (paid by
 * hand, or imported from the bank) within 12 days. The balance entered on a
 * day already includes that day's rule dates. A movement dated in the future
 * is planned (the 730 refund in July, the car tax): it counts when its day
 * comes, and in the period's sums before that.
 *
 * The period. It runs from today to the next payday of the main income (the
 * largest monthly income), today included and payday excluded: what can be
 * spent is the balance, minus expenses due in the period, plus other income
 * expected in it; spread over its days that is the daily budget.
 */

#include <stdbool.h>
#include <stdint.h>

typedef int64_t bl_cents_t;
typedef int32_t bl_day_t;

#define BL_NONE (-1)
#define BL_NAME 32
#define BL_NOTE 48
#define BL_ANY_CATEGORY 255
#define BL_LINK_DAYS 12           /* a movement linked to a rule pays the date within this many days */
#define BL_PACE_DAYS 30           /* the spending pace looks at the last 30 days */
#define BL_FIXED_ALARM_PCT 50     /* fixed costs above half the income: a warning */

enum { BL_INCOME, BL_EXPENSE };
enum { BL_SHIFT_EXACT, BL_SHIFT_BEFORE, BL_SHIFT_AFTER };
enum { BL_OK, BL_FAST, BL_SHORT };   /* summary state: in line, above the pace, heading below zero */
enum { BL_LOAN_NONE, BL_LOAN_MORTGAGE, BL_LOAN_PERSONAL, BL_LOAN_CAR, BL_LOAN_FIFTH, BL_LOAN_LEASING,
       BL_LOAN_INSTALMENTS, BL_LOAN_OTHER, BL_LOAN_TYPES };
enum { BL_UNIT_NONE, BL_UNIT_KWH, BL_UNIT_SMC, BL_UNIT_M3, BL_UNITS };

/*
 * A debt paid by the rule's instalments (mortgage, loan, car finance, salary
 * assignment, leasing, buy now pay later...). French amortisation: equal
 * instalments, interest on the remaining capital. Anything unknown is 0 / -1
 * and is worked out from the rest when possible (the rate from the amount
 * borrowed, or from a bank statement's residual debt).
 */
typedef struct
{
    uint8_t type;           /* BL_LOAN_* */
    int32_t tan;            /* yearly nominal rate in thousandths of a percent (3,25% = 3250), -1 unknown */
    bl_cents_t principal;   /* amount borrowed, 0 unknown */
    bl_cents_t balloon;     /* final lump with the last instalment (maxi-rata, riscatto), 0 none */
    bl_cents_t debt;        /* residual capital from a bank statement, 0 none... */
    bl_day_t debt_date;     /* ...on this day (instalments up to it are paid), BL_NONE none */
} bl_loan_t;

/*
 * A bill that depends on consumption (electricity, gas, water): a fixed part
 * per month plus a price per unit, taxes included. The next bill is
 * estimated from the recorded consumption, month by month (winter gas).
 */
typedef struct
{
    uint8_t unit;           /* BL_UNIT_*, NONE = a fixed amount */
    int64_t price;          /* per unit, millionths of a euro (0,285 €/kWh = 285000) */
    bl_cents_t fixed;       /* per month */
    int64_t meter;          /* last meter reading, thousandths of a unit (0 none) */
    bl_day_t meter_date;
} bl_usage_t;

/* Consumption of one rule over [from, to), e.g. a bill's period or two meter readings. */
typedef struct
{
    uint32_t id;
    uint32_t rule;
    bl_day_t from;
    bl_day_t to;
    int64_t quantity;       /* thousandths of a unit */
} bl_period_t;

typedef struct
{
    uint32_t id;
    uint8_t kind;           /* BL_INCOME / BL_EXPENSE */
    uint8_t every;          /* months between dates: 1, 2, 3, 4, 6, 12, 24, 36, 60, 120 */
    uint8_t month;          /* 1..12: a month in which it falls (1 < every <= 12) */
    uint8_t day;            /* 1..31 */
    uint8_t shift;          /* BL_SHIFT_* */
    uint8_t category;
    int32_t first;          /* first month, 0 = since always */
    int32_t last;           /* last month (included), 0 = no end */
    bl_cents_t amount;      /* positive; for a usage bill the current estimate */
    bl_loan_t loan;
    bl_usage_t usage;
    char name[BL_NAME];
} bl_rule_t;

typedef struct
{
    int total;              /* instalments, -1 without a first month */
    int paid;               /* up to today, -1 without a first month */
    int left;               /* after today */
    bl_day_t start;         /* first instalment, BL_NONE unknown */
    bl_day_t end;           /* last instalment */
    bl_cents_t paid_amount; /* instalments (and balloon) already paid, -1 unknown */
    bl_cents_t left_amount; /* still to pay, balloon included */
    bl_cents_t principal;   /* borrowed: given or worked out, -1 unknown */
    bl_cents_t debt;        /* residual capital today, -1 unknown */
    bl_cents_t interest_left;  /* left_amount - debt, -1 unknown */
    bl_cents_t interest_total; /* everything paid over the life - principal, -1 unknown */
    int32_t tan;            /* given or worked out, -1 unknown */
    bool tan_derived;
} bl_loan_status_t;

typedef struct
{
    uint32_t id;
    bl_day_t date;
    bl_cents_t amount;      /* < 0 out, > 0 in */
    uint32_t rule;          /* the rule date this movement pays or receives, 0 = none */
    uint8_t category;
    char note[BL_NOTE];
} bl_tx_t;

typedef struct
{
    bl_cents_t balance;
    bl_day_t balance_date;  /* BL_NONE: never entered */
    uint32_t balance_after; /* movements with a higher id were recorded after it */
    const bl_rule_t *rules;
    int rule_count;
    const bl_tx_t *tx;
    int tx_count;
} bl_book_t;

typedef struct
{
    bool has_balance;
    bl_cents_t balance;         /* estimated today */
    int main_income;            /* index of the main income rule, -1 = none */
    bl_day_t period_start;      /* last payday (or first of the month) */
    bl_day_t payday;            /* next payday (or first of next month) */
    int days;                   /* payday - today, at least 1 */
    bl_cents_t due;             /* expenses due in [today, payday) not paid yet */
    bl_cents_t income;          /* other income expected in (today, payday) */
    bl_cents_t free;            /* balance - due + income */
    bl_cents_t daily;           /* budget per day, set at the start of today */
    bl_cents_t spent_today;     /* variable spending recorded today */
    bl_cents_t today_left;      /* daily - spent_today (may be < 0) */
    bl_cents_t pace;            /* average variable spending per day */
    bl_cents_t forecast;        /* balance expected the day before payday at that pace */
    bl_cents_t period_spent;    /* variable spending since period_start */
    int state;                  /* BL_OK / BL_FAST / BL_SHORT */
} bl_summary_t;

/* ------------------------------------------------------------- calendar */

bl_day_t bl_days_from_civil(int year, int month, int day);
void bl_civil(bl_day_t days, int *year, int *month, int *day);
int bl_weekday(bl_day_t days);          /* 0 Monday .. 6 Sunday */
int bl_days_in_month(int year, int month);
int bl_month_index(int year, int month);
int bl_month_of(bl_day_t days);
bl_day_t bl_month_start(int month_index);

/* "2026-10-17" -> day, "2028-06" -> month index; BL_NONE / 0 if malformed. */
bl_day_t bl_parse_date(const char *text);
int bl_parse_month(const char *text);
/* out: 11 bytes ("2026-10-17") / 8 bytes ("2028-06"). */
void bl_format_date(bl_day_t days, char *out);
void bl_format_month(int month_index, char *out);

/* Euros (a JSON number) to cents, rounded to the nearest cent. */
bl_cents_t bl_cents(double euros);

/* ---------------------------------------------------------------- rules */

bl_day_t bl_shift(bl_day_t days, int shift);
bool bl_rule_in_month(const bl_rule_t *rule, int month_index);
/* The rule's date for a nominal month (shift applied), BL_NONE if none. */
bl_day_t bl_occurrence(const bl_rule_t *rule, int month_index);
/* First date strictly after `after`, BL_NONE if the rule has ended. */
bl_day_t bl_next(const bl_rule_t *rule, bl_day_t after);
/* Last date on or before `day`, BL_NONE if none. */
bl_day_t bl_previous(const bl_rule_t *rule, bl_day_t day);

/* Calls visit(rule, date, ctx) for every date in [from, to), in month order per rule. */
typedef void (*bl_visit_t)(const bl_rule_t *rule, bl_day_t date, void *ctx);
void bl_each_date(const bl_rule_t *rules, int count, bl_day_t from, bl_day_t to, bl_visit_t visit, void *ctx);

/* A movement linked to this rule pays the date (within BL_LINK_DAYS). */
bool bl_paid(const bl_book_t *book, const bl_rule_t *rule, bl_day_t date);

/* The largest monthly income (else the largest income), -1 if none. */
int bl_main_income(const bl_rule_t *rules, int count);
/* Next payday strictly after today: the main income's next date, else the 1st of next month. */
bl_day_t bl_next_payday(const bl_book_t *book, bl_day_t today);

/* ------------------------------------------------------------- analysis */

bl_cents_t bl_balance_today(const bl_book_t *book, bl_day_t today);
/* Unpaid dates of `kind` in [from, to) (only after the balance day). */
bl_cents_t bl_due_between(const bl_book_t *book, int kind, bl_day_t from, bl_day_t to);
void bl_summary(const bl_book_t *book, bl_day_t today, bl_summary_t *out);

/* Rule dates of `kind` whose nominal month is `month_index` (what falls this month). */
bl_cents_t bl_month_total(const bl_rule_t *rules, int count, int kind, int month_index);
/* Monthly share of every active rule of `kind` (a yearly 540 -> 45 a month). */
bl_cents_t bl_monthly_share(const bl_rule_t *rules, int count, int kind, int month_index);
/* Fixed costs as a percentage of income (monthly shares), -1 without income. */
int bl_fixed_ratio(const bl_rule_t *rules, int count, int month_index);
/* Yearly cost of the active rules of a category ("abbonamenti"), BL_ANY_CATEGORY for all. */
bl_cents_t bl_yearly_cost(const bl_rule_t *rules, int count, int category, int month_index);
/* What to put aside every month for the expenses that are not monthly (insurance, condominio...). */
bl_cents_t bl_set_aside(const bl_rule_t *rules, int count, int month_index);
/*
 * A loan or any rule with an end: dates already passed (up to today, -1
 * without a first month) and still to come (-1 without an end).
 * Returns the money still to pay (remaining dates * amount), 0 without end.
 */
bl_cents_t bl_rule_progress(const bl_rule_t *rule, bl_day_t today, int *done, int *left);

/* Variable spending (movements out, not linked to rules) of a category in [from, to), positive. */
bl_cents_t bl_spent(const bl_book_t *book, int category, bl_day_t from, bl_day_t to);
/* Average variable spending per day over the last BL_PACE_DAYS before today. */
bl_cents_t bl_pace(const bl_book_t *book, bl_day_t today);
/*
 * A category this month (1st to today) against the average of the same days
 * of the previous 3 months (only months with movements count).
 * Returns the percentage change, or a value < -100 with no history.
 */
int bl_category_trend(const bl_book_t *book, int category, bl_day_t today, bl_cents_t *now, bl_cents_t *average);

/* Loans (rate, residual capital) and usage bills: types above, maths planned (BILANCIO.md, passo 2b). */

#endif
