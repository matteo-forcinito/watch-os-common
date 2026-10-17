/* Bilancio logic tests. Returns the failing line, 0 if all pass. */
#include "calc.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static bl_day_t D(int y, int m, int d) { return bl_days_from_civil(y, m, d); }

static bl_rule_t rule(uint32_t id, int kind, bl_cents_t amount, int every, int month, int day, int shift, int category)
{
    bl_rule_t r = {0};
    r.id = id;
    r.kind = (uint8_t)kind;
    r.amount = amount;
    r.every = (uint8_t)every;
    r.month = (uint8_t)month;
    r.day = (uint8_t)day;
    r.shift = (uint8_t)shift;
    r.category = (uint8_t)category;
    return r;
}

static bl_tx_t tx(uint32_t id, bl_day_t date, bl_cents_t amount, int category, uint32_t linked)
{
    bl_tx_t t = {0};
    t.id = id;
    t.date = date;
    t.amount = amount;
    t.category = (uint8_t)category;
    t.rule = linked;
    return t;
}

enum { CASA, AUTO, SPESA, ABBONAMENTI, SVAGO };

int bilancio_tests(void)
{
    char text[12];

    /* Calendar. */
    CHECK(D(1970, 1, 1) == 0 && D(2026, 10, 17) == 20743 && D(2000, 3, 1) == 11017);
    CHECK(bl_weekday(D(2026, 10, 17)) == 5 && bl_weekday(D(2026, 11, 1)) == 6 && bl_weekday(D(1970, 1, 5)) == 0);
    for (bl_day_t day = D(2023, 12, 1); day < D(2029, 3, 1); day += 17)
    {
        int y, m, d;
        bl_civil(day, &y, &m, &d);
        CHECK(D(y, m, d) == day);
    }
    CHECK(bl_days_in_month(2024, 2) == 29 && bl_days_in_month(2026, 2) == 28 && bl_days_in_month(2100, 2) == 28);
    CHECK(bl_parse_date("2026-10-17") == D(2026, 10, 17) && bl_parse_date("2024-02-29") == D(2024, 2, 29));
    CHECK(bl_parse_date("2026-02-29") == BL_NONE && bl_parse_date("2026-13-01") == BL_NONE);
    CHECK(bl_parse_date("2026-10-1") == BL_NONE && bl_parse_date("2026-10-17x") == BL_NONE && bl_parse_date("") == BL_NONE);
    CHECK(bl_parse_month("2028-06") == bl_month_index(2028, 6) && bl_parse_month("2028-6") == 0 && bl_parse_month(0) == 0);
    bl_format_date(D(2026, 1, 5), text);
    CHECK(eq(text, "2026-01-05"));
    bl_format_month(bl_month_index(2028, 6), text);
    CHECK(eq(text, "2028-06"));

    /* Money: whole cents, rounded. */
    CHECK(bl_cents(13.99) == 1399 && bl_cents(-42.30) == -4230 && bl_cents(1240.50) == 124050);
    CHECK(bl_cents(0.1 + 0.2) == 30 && bl_cents(0) == 0 && bl_cents(-0.004) == 0);

    /* Salary on the 27th: Saturday 27/2/2027 -> Friday 26, or Monday 1/3 with "after". */
    bl_rule_t salary = rule(1, BL_INCOME, 185000, 1, 0, 27, BL_SHIFT_BEFORE, 0);
    CHECK(bl_occurrence(&salary, bl_month_index(2027, 2)) == D(2027, 2, 26));
    salary.shift = BL_SHIFT_AFTER;
    CHECK(bl_occurrence(&salary, bl_month_index(2027, 2)) == D(2027, 3, 1));
    salary.shift = BL_SHIFT_EXACT;
    CHECK(bl_occurrence(&salary, bl_month_index(2027, 2)) == D(2027, 2, 27));
    salary.shift = BL_SHIFT_BEFORE;
    CHECK(bl_occurrence(&salary, bl_month_index(2026, 12)) == D(2026, 12, 25));   /* Sunday 27 */
    CHECK(bl_next(&salary, D(2026, 10, 17)) == D(2026, 10, 27));
    CHECK(bl_next(&salary, D(2026, 10, 27)) == D(2026, 11, 27));
    CHECK(bl_previous(&salary, D(2026, 10, 27)) == D(2026, 10, 27) && bl_previous(&salary, D(2026, 10, 26)) == D(2026, 9, 25));

    /* Paid on the 1st, Sunday 1/11/2026 with "before": Friday 30 October. */
    bl_rule_t first = rule(2, BL_INCOME, 100000, 1, 0, 1, BL_SHIFT_BEFORE, 0);
    CHECK(bl_occurrence(&first, bl_month_index(2026, 11)) == D(2026, 10, 30));
    CHECK(bl_next(&first, D(2026, 10, 17)) == D(2026, 10, 30));
    CHECK(bl_next(&first, D(2026, 10, 30)) == D(2026, 12, 1));

    /* Day 31 in a short month: the last day. */
    bl_rule_t end_of_month = rule(3, BL_EXPENSE, 1000, 1, 0, 31, BL_SHIFT_EXACT, CASA);
    CHECK(bl_occurrence(&end_of_month, bl_month_index(2027, 2)) == D(2027, 2, 28));
    CHECK(bl_occurrence(&end_of_month, bl_month_index(2028, 2)) == D(2028, 2, 29));

    /* Yearly insurance in March only. */
    bl_rule_t insurance = rule(12, BL_EXPENSE, 54000, 12, 3, 15, BL_SHIFT_EXACT, AUTO);
    CHECK(bl_rule_in_month(&insurance, bl_month_index(2027, 3)) && !bl_rule_in_month(&insurance, bl_month_index(2027, 4)));
    CHECK(bl_next(&insurance, D(2026, 10, 17)) == D(2027, 3, 15));
    CHECK(bl_month_total(&insurance, 1, BL_EXPENSE, bl_month_index(2027, 3)) == 54000);
    CHECK(bl_month_total(&insurance, 1, BL_EXPENSE, bl_month_index(2027, 2)) == 0);

    /* Condominio every 3 months from January: Jan, Apr, Jul, Oct. */
    bl_rule_t condo = rule(14, BL_EXPENSE, 36000, 3, 1, 10, BL_SHIFT_EXACT, CASA);
    CHECK(bl_rule_in_month(&condo, bl_month_index(2026, 10)) && bl_rule_in_month(&condo, bl_month_index(2027, 1)));
    CHECK(!bl_rule_in_month(&condo, bl_month_index(2026, 11)) && !bl_rule_in_month(&condo, bl_month_index(2026, 12)));
    CHECK(bl_next(&condo, D(2026, 10, 17)) == D(2027, 1, 10));   /* a Sunday: "exact" keeps it */

    /* Car inspection every 2 years from May 2027, driving licence every 10 years. */
    bl_rule_t inspection = rule(16, BL_EXPENSE, 7900, 24, 1, 20, BL_SHIFT_EXACT, AUTO);
    inspection.first = bl_month_index(2027, 5);
    CHECK(bl_rule_in_month(&inspection, bl_month_index(2027, 5)) && !bl_rule_in_month(&inspection, bl_month_index(2028, 5)));
    CHECK(bl_rule_in_month(&inspection, bl_month_index(2029, 5)) && bl_rule_in_month(&inspection, bl_month_index(2025, 5)));
    CHECK(!bl_rule_in_month(&inspection, bl_month_index(2026, 5)));
    CHECK(bl_next(&inspection, D(2026, 10, 17)) == D(2027, 5, 20) && bl_next(&inspection, D(2027, 5, 20)) == D(2029, 5, 20));
    CHECK(bl_previous(&inspection, D(2030, 1, 1)) == D(2029, 5, 20));
    bl_rule_t licence = rule(17, BL_EXPENSE, 12000, 120, 1, 3, BL_SHIFT_EXACT, AUTO);
    licence.first = bl_month_index(2031, 3);
    CHECK(bl_next(&licence, D(2026, 10, 17)) == D(2031, 3, 3) && bl_next(&licence, D(2031, 3, 3)) == D(2041, 3, 3));
    /* Set aside from now on: 120 euros in 10 years = 1 euro a month. */
    CHECK(bl_monthly_share(&licence, 1, BL_EXPENSE, bl_month_index(2026, 10)) == 100);
    CHECK(bl_set_aside(&licence, 1, bl_month_index(2026, 10)) == 100);

    /* Car loan ending June 2028: counts up to then, never after. */
    bl_rule_t loan = rule(10, BL_EXPENSE, 28900, 1, 0, 5, BL_SHIFT_EXACT, AUTO);
    loan.first = bl_month_index(2024, 7);
    loan.last = bl_month_index(2028, 6);
    CHECK(bl_rule_in_month(&loan, bl_month_index(2028, 6)) && !bl_rule_in_month(&loan, bl_month_index(2028, 7)));
    CHECK(!bl_rule_in_month(&loan, bl_month_index(2024, 6)));
    CHECK(bl_next(&loan, D(2028, 6, 10)) == BL_NONE);
    CHECK(bl_month_total(&loan, 1, BL_EXPENSE, bl_month_index(2028, 8)) == 0);
    int done, left;
    CHECK(bl_rule_progress(&loan, D(2026, 10, 17), &done, &left) == 20 * 28900 && done == 28 && left == 20);
    CHECK(bl_rule_progress(&salary, D(2026, 10, 17), &done, &left) == 0 && done == -1 && left == -1);

    /* The household of the design document, on Saturday 17/10/2026. */
    bl_rule_t rules[] = {
        rule(1, BL_INCOME, 185000, 1, 0, 27, BL_SHIFT_BEFORE, 0),
        loan,
        rule(11, BL_EXPENSE, 65000, 1, 0, 1, BL_SHIFT_EXACT, CASA),
        insurance,
        rule(13, BL_EXPENSE, 1399, 1, 0, 20, BL_SHIFT_EXACT, ABBONAMENTI),
        rule(15, BL_INCOME, 185000, 12, 12, 15, BL_SHIFT_BEFORE, 0),      /* tredicesima */
    };
    bl_tx_t moves[8];
    bl_book_t book = {.balance = 124050, .balance_date = D(2026, 10, 17), .balance_after = 100,
                      .rules = rules, .rule_count = 6, .tx = moves, .tx_count = 0};
    moves[book.tx_count++] = tx(100, D(2026, 10, 15), -4230, SPESA, 0);   /* before the balance: in it */
    CHECK(bl_main_income(rules, 6) == 0);
    CHECK(bl_next_payday(&book, D(2026, 10, 17)) == D(2026, 10, 27));

    bl_summary_t s;
    bl_summary(&book, D(2026, 10, 17), &s);
    CHECK(s.has_balance && s.balance == 124050 && s.payday == D(2026, 10, 27) && s.days == 10);
    CHECK(s.period_start == D(2026, 9, 25));                 /* Sunday 27/9 -> Friday 25 */
    CHECK(s.due == 1399 && s.income == 0 && s.free == 122651 && s.daily == 12265 && s.today_left == 12265);
    CHECK(s.state == BL_OK);

    /* A coffee today: the day's budget stays, what is left goes down. */
    moves[book.tx_count++] = tx(101, D(2026, 10, 17), -1500, SVAGO, 0);
    bl_summary(&book, D(2026, 10, 17), &s);
    CHECK(s.balance == 122550 && s.spent_today == 1500 && s.daily == 12265 && s.today_left == 10765);

    /* Five days later Netflix (20/10) has gone out by itself. */
    bl_summary(&book, D(2026, 10, 22), &s);
    CHECK(s.balance == 122550 - 1399 && s.due == 0 && s.days == 5);
    /* ...unless it was recorded by hand: then it counts once. */
    moves[book.tx_count++] = tx(102, D(2026, 10, 21), -1399, ABBONAMENTI, 13);
    bl_summary(&book, D(2026, 10, 22), &s);
    CHECK(s.balance == 122550 - 1399 && s.spent_today == 0);
    CHECK(bl_paid(&book, &rules[4], D(2026, 10, 20)) && !bl_paid(&book, &rules[4], D(2026, 11, 20)));

    /* After payday: the salary is in, the next period begins. */
    bl_summary(&book, D(2026, 10, 28), &s);
    CHECK(s.balance == 122550 - 1399 + 185000 && s.period_start == D(2026, 10, 27) && s.payday == D(2026, 11, 27));
    CHECK(s.due == 65000 + 28900 + 1399 && s.days == 30);

    /* A planned 730 refund before payday raises the budget, not the balance. */
    moves[book.tx_count++] = tx(103, D(2026, 10, 25), 45000, 0, 0);
    bl_summary(&book, D(2026, 10, 17), &s);
    CHECK(s.balance == 122550 && s.income == 45000 && s.free == 122550 - 1399 + 45000);
    book.tx_count--;

    /* Negative balance: budget 0 and the problem flagged. */
    book.balance = -20000;
    bl_summary(&book, D(2026, 10, 17), &s);
    CHECK(s.daily == 0 && s.free < 0 && s.state == BL_SHORT && s.today_left == -1500);
    book.balance = 124050;

    /* No balance entered: income minus expenses since the last payday (25/9). */
    bl_book_t empty = {.balance_date = BL_NONE, .rules = rules, .rule_count = 1};
    bl_summary(&empty, D(2026, 10, 17), &s);
    CHECK(s.has_balance && s.balance == 185000 && s.daily == 18500 && s.payday == D(2026, 10, 27));
    /* ...with the rent of 1/10 and a movement of the 2nd: both counted. */
    bl_tx_t one[] = {tx(1, D(2026, 10, 2), -5000, SPESA, 0)};
    bl_book_t computed = {.balance_date = BL_NONE, .rules = rules, .rule_count = 3, .tx = one, .tx_count = 1};
    CHECK(bl_balance_today(&computed, D(2026, 10, 17)) == 185000 - 65000 - 28900 - 5000);   /* salary, rent, loan 5/10 */
    bl_book_t nothing = {.balance_date = BL_NONE};
    bl_summary(&nothing, D(2026, 10, 17), &s);
    CHECK(!s.has_balance && s.balance == 0 && s.daily == 0);
    /* No income at all: the period ends with the month. */
    bl_book_t no_income = {.balance = 31000, .balance_date = D(2026, 10, 17), .rules = &rules[2], .rule_count = 1};
    bl_summary(&no_income, D(2026, 10, 17), &s);
    CHECK(s.payday == D(2026, 11, 1) && s.days == 15 && s.period_start == D(2026, 10, 1));
    CHECK(s.due == 0 && s.daily == 31000 / 15);

    /* Fixed costs against income: monthly shares (insurance 540/12 = 45). */
    int october = bl_month_index(2026, 10);
    CHECK(bl_monthly_share(rules, 6, BL_EXPENSE, october) == 28900 + 65000 + 4500 + 1399);
    CHECK(bl_monthly_share(rules, 6, BL_INCOME, october) == 185000 + 185000 / 12);
    CHECK(bl_fixed_ratio(rules, 6, october) == (int)((28900 + 65000 + 4500 + 1399) * 100 / (185000 + 15416)));
    CHECK(bl_fixed_ratio(&rules[1], 2, october) == -1);
    CHECK(bl_yearly_cost(rules, 6, ABBONAMENTI, october) == 1399 * 12);
    CHECK(bl_set_aside(rules, 6, october) == 4500);
    CHECK(bl_monthly_share(rules, 6, BL_EXPENSE, bl_month_index(2028, 7)) == 65000 + 4500 + 1399);

    /* Spending pace and category trend: svago +40% on the average of 3 months. */
    bl_tx_t history[] = {
        tx(200, D(2026, 6, 30), -100, SPESA, 0),
        tx(201, D(2026, 7, 5), -10000, SVAGO, 0),
        tx(202, D(2026, 8, 5), -10000, SVAGO, 0),
        tx(203, D(2026, 8, 20), -50000, SVAGO, 0),          /* after the 17th: outside the window */
        tx(204, D(2026, 9, 5), -10000, SVAGO, 0),
        tx(205, D(2026, 10, 5), -14000, SVAGO, 0),
        tx(206, D(2026, 10, 6), -3000, SPESA, 13),          /* linked to a rule: not variable */
        tx(207, D(2026, 10, 10), 5000, SVAGO, 0),           /* money in: not spending */
    };
    bl_book_t past = {.balance_date = BL_NONE, .tx = history, .tx_count = 8};
    bl_cents_t now, average;
    CHECK(bl_category_trend(&past, SVAGO, D(2026, 10, 17), &now, &average) == 40 && now == 14000 && average == 10000);
    CHECK(bl_category_trend(&past, CASA, D(2026, 10, 17), &now, &average) == 0);
    CHECK(bl_spent(&past, BL_ANY_CATEGORY, D(2026, 10, 1), D(2026, 11, 1)) == 14000);
    CHECK(bl_pace(&past, D(2026, 10, 17)) == 14000 / 30);           /* the last 30 days only */
    bl_tx_t fresh[] = {tx(300, D(2026, 10, 15), -7000, SPESA, 0)};
    bl_book_t young = {.balance_date = BL_NONE, .tx = fresh, .tx_count = 1};
    CHECK(bl_pace(&young, D(2026, 10, 17)) == 1000);     /* 2 days of data count as 7 */
    CHECK(bl_category_trend(&young, SPESA, D(2026, 10, 17), &now, &average) < -100);
    return 0;
}

#ifdef BILANCIO_EMULATOR_ENTRY
/* The compiler may call these for struct copies; there is no libc in the emulator. */
void *memcpy(void *out, const void *in, unsigned long size)
{
    char *o = out;
    const char *i = in;
    while (size--) *o++ = *i++;
    return out;
}

void *memset(void *out, int value, unsigned long size)
{
    char *o = out;
    while (size--) *o++ = (char)value;
    return out;
}

__attribute__((naked)) void _start(void)
{
    /* Full-size ebreak: the runner looks for the 4-byte instruction. */
    __asm__ volatile(".option push\n .option norvc\n call bilancio_tests\n ebreak\n .option pop\n");
}
#else
#include <stdio.h>
int main(void)
{
    int line = bilancio_tests();
    if (line) printf("FAIL at line %d\n", line);
    return line;
}
#endif
