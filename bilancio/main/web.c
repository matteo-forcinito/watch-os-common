/*
 * Bilancio dashboard API (served by appkit; every route needs the paired token).
 * Money is in cents (integers), dates "YYYY-MM-DD", months "YYYY-MM".
 *
 *   GET  /api/state        today's answers: summary, what is coming, trends, loans
 *   GET  /api/rules        income and fixed costs
 *   GET  /api/tx           ?month=2026-10 -> that month's movements, newest first
 *   GET  /api/months       the last 24 months: in, out, count (+ archive)
 *   GET  /api/export       the whole book as the JSON file
 *   POST /api/tx           {id?, date, amount, category, note, rule}
 *   POST /api/tx/delete    {id}
 *   POST /api/rule         {id?, kind, name, amount, every, month, day, shift, category, first, last}
 *   POST /api/rule/delete  {id}
 *   POST /api/balance      {amount, date}
 *   POST /api/category     {index (-1 new), name}
 *   POST /api/import       the JSON file (replaces everything)
 *
 * The calculations are calc.c's, the same the watch shows.
 */
#include "bilancio.h"

#include "appkit.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char dashboard_html_start[] asm("_binary_bilancio_dashboard_html_start");
extern const char dashboard_html_end[] asm("_binary_bilancio_dashboard_html_end");

#define GUARD() do { if (!appkit_authorized(request)) return appkit_unauthorized(request); } while (0)
#define TIMELINE_MAX 160
#define TIMELINE_AHEAD 35         /* days past payday */
#define RULE_HISTORY 12           /* payments shown per rule */

static double num(const cJSON *o, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback;
}

static const char *text(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static cJSON *body_json(httpd_req_t *request)
{
    char *raw = appkit_body(request, 4096);
    cJSON *root = raw != NULL ? cJSON_Parse(raw) : NULL;
    free(raw);
    return root;
}

static esp_err_t bad(httpd_req_t *request, const char *why)
{
    return appkit_error(request, "400 Bad Request", why);
}

static esp_err_t ok_id(httpd_req_t *request, uint32_t id)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "id", id);
    return appkit_json(request, out);
}

static void add_date(cJSON *o, const char *key, bl_day_t day)
{
    char t[12];
    if (day == BL_NONE)
    {
        cJSON_AddNullToObject(o, key);
        return;
    }
    bl_format_date(day, t);
    cJSON_AddStringToObject(o, key, t);
}

static void add_month(cJSON *o, const char *key, int month)
{
    char t[8] = "";
    if (month != 0)
    {
        bl_format_month(month, t);
    }
    cJSON_AddStringToObject(o, key, t);
}

static void add_money(cJSON *o, const char *key, bl_cents_t cents)
{
    cJSON_AddNumberToObject(o, key, (double)cents);
}

static cJSON *rule_json(const bl_rule_t *r)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "id", r->id);
    cJSON_AddNumberToObject(j, "kind", r->kind);
    cJSON_AddStringToObject(j, "name", r->name);
    add_money(j, "amount", r->amount);
    cJSON_AddNumberToObject(j, "every", r->every);
    cJSON_AddNumberToObject(j, "month", r->month);
    cJSON_AddNumberToObject(j, "day", r->day);
    cJSON_AddNumberToObject(j, "shift", r->shift);
    cJSON_AddNumberToObject(j, "category", r->category);
    add_month(j, "first", r->first);
    add_month(j, "last", r->last);
    return j;
}

static cJSON *tx_json(const bl_tx_t *t)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "id", t->id);
    add_date(j, "date", t->date);
    add_money(j, "amount", t->amount);
    cJSON_AddNumberToObject(j, "category", t->category);
    cJSON_AddStringToObject(j, "note", t->note);
    cJSON_AddNumberToObject(j, "rule", t->rule);
    return j;
}

/* ---------------------------------------------------------------- state */

typedef struct
{
    const bl_rule_t *rule;
    bl_day_t date;
} dated_t;

typedef struct
{
    dated_t *items;
    int count;
} timeline_t;

static void collect(const bl_rule_t *rule, bl_day_t date, void *ctx)
{
    timeline_t *t = ctx;
    if (t->count < TIMELINE_MAX)
    {
        t->items[t->count++] = (dated_t){rule, date};
    }
}

static int by_date(const void *a, const void *b)
{
    const dated_t *x = a, *y = b;
    return x->date != y->date ? (x->date < y->date ? -1 : 1) : (x->rule->amount > y->rule->amount ? -1 : 1);
}

static esp_err_t state(httpd_req_t *request)
{
    GUARD();
    bl_day_t today = bl_today();
    if (today == BL_NONE)
    {
        return appkit_error(request, "503 Service Unavailable", "L'orologio non ha ancora l'ora: collegalo al Wi-Fi");
    }
    dated_t *items = heap_caps_malloc(sizeof(dated_t) * TIMELINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (items == NULL)
    {
        return appkit_json(request, NULL);
    }
    cJSON *out = cJSON_CreateObject();
    int month = bl_month_of(today);
    bl_lock();
    const bl_book_t *book = bl_book();
    bl_summary_t s;
    bl_summary(book, today, &s);

    add_date(out, "today", today);
    cJSON_AddNumberToObject(out, "revision", bl_revision());
    cJSON_AddStringToObject(out, "currency", "EUR");
    cJSON *balance = cJSON_AddObjectToObject(out, "balance");
    cJSON_AddBoolToObject(balance, "set", s.has_balance);
    add_money(balance, "amount", book->balance);
    add_date(balance, "date", book->balance_date);

    cJSON *sum = cJSON_AddObjectToObject(out, "summary");
    add_money(sum, "balance", s.balance);
    add_date(sum, "period_start", s.period_start);
    add_date(sum, "payday", s.payday);
    cJSON_AddNumberToObject(sum, "days", s.days);
    add_money(sum, "due", s.due);
    add_money(sum, "income", s.income);
    add_money(sum, "free", s.free);
    add_money(sum, "daily", s.daily);
    add_money(sum, "spent_today", s.spent_today);
    add_money(sum, "today_left", s.today_left);
    add_money(sum, "pace", s.pace);
    add_money(sum, "forecast", s.forecast);
    add_money(sum, "period_spent", s.period_spent);
    cJSON_AddNumberToObject(sum, "state", s.state);
    cJSON_AddNumberToObject(sum, "main_income", s.main_income >= 0 ? book->rules[s.main_income].id : 0);

    cJSON *m = cJSON_AddObjectToObject(out, "month");
    add_money(m, "fixed", bl_monthly_share(book->rules, book->rule_count, BL_EXPENSE, month));
    add_money(m, "income", bl_monthly_share(book->rules, book->rule_count, BL_INCOME, month));
    cJSON_AddNumberToObject(m, "fixed_ratio", bl_fixed_ratio(book->rules, book->rule_count, month));
    add_money(m, "due_total", bl_month_total(book->rules, book->rule_count, BL_EXPENSE, month));
    add_money(m, "set_aside", bl_set_aside(book->rules, book->rule_count, month));
    add_money(m, "yearly_fixed", bl_yearly_cost(book->rules, book->rule_count, BL_ANY_CATEGORY, month));
    add_money(m, "spent", bl_spent(book, BL_ANY_CATEGORY, bl_month_start(month), today + 1));

    /* What falls from the last payday to a month past the next one. */
    timeline_t t = {.items = items};
    bl_each_date(book->rules, book->rule_count, s.period_start, s.payday + TIMELINE_AHEAD, collect, &t);
    qsort(items, (size_t)t.count, sizeof(dated_t), by_date);
    cJSON *timeline = cJSON_AddArrayToObject(out, "timeline");
    for (int i = 0; i < t.count; i++)
    {
        const bl_rule_t *r = items[i].rule;
        bl_day_t date = items[i].date;
        bool paid = bl_paid(book, r, date);
        bool done = paid || date < today || (r->kind == BL_INCOME && date == today) ||
                    (book->balance_date != BL_NONE && date <= book->balance_date);
        cJSON *j = cJSON_CreateObject();
        add_date(j, "date", date);
        cJSON_AddNumberToObject(j, "rule", r->id);
        cJSON_AddStringToObject(j, "name", r->name);
        add_money(j, "amount", r->kind == BL_INCOME ? r->amount : -r->amount);
        cJSON_AddNumberToObject(j, "category", r->category);
        cJSON_AddBoolToObject(j, "done", done);
        cJSON_AddBoolToObject(j, "linked", paid);
        cJSON_AddItemToArray(timeline, j);
    }
    /* Planned movements (dated in the future). */
    cJSON *planned = cJSON_AddArrayToObject(out, "planned");
    for (int i = 0; i < book->tx_count; i++)
    {
        if (book->tx[i].date > today)
        {
            cJSON_AddItemToArray(planned, tx_json(&book->tx[i]));
        }
    }
    /* Non-monthly costs: when they come and what to set aside for them. */
    cJSON *yearly = cJSON_AddArrayToObject(out, "upcoming");
    for (int i = 0; i < book->rule_count; i++)
    {
        const bl_rule_t *r = &book->rules[i];
        bl_day_t next = r->kind == BL_EXPENSE && r->every > 1 ? bl_next(r, today - 1) : BL_NONE;
        if (next != BL_NONE)
        {
            cJSON *j = cJSON_CreateObject();
            cJSON_AddNumberToObject(j, "rule", r->id);
            cJSON_AddStringToObject(j, "name", r->name);
            add_date(j, "date", next);
            add_money(j, "amount", r->amount);
            cJSON_AddNumberToObject(j, "every", r->every);
            cJSON_AddNumberToObject(j, "category", r->category);
            cJSON_AddItemToArray(yearly, j);
        }
    }
    /* Loans and anything with an end. */
    cJSON *loans = cJSON_AddArrayToObject(out, "loans");
    for (int i = 0; i < book->rule_count; i++)
    {
        const bl_rule_t *r = &book->rules[i];
        int done, left;
        bl_cents_t remaining = bl_rule_progress(r, today, &done, &left);
        if (r->kind == BL_EXPENSE && left >= 0)
        {
            cJSON *j = cJSON_CreateObject();
            cJSON_AddNumberToObject(j, "rule", r->id);
            cJSON_AddStringToObject(j, "name", r->name);
            add_money(j, "amount", r->amount);
            add_money(j, "remaining", remaining);
            cJSON_AddNumberToObject(j, "done", done);
            cJSON_AddNumberToObject(j, "left", left);
            add_month(j, "last", r->last);
            cJSON_AddNumberToObject(j, "category", r->category);
            cJSON_AddItemToArray(loans, j);
        }
    }
    /* Variable spending by category: this month against the usual. */
    int category_count;
    const char (*names)[BL_CATEGORY_LEN] = bl_categories(&category_count);
    cJSON *categories = cJSON_AddArrayToObject(out, "categories");
    cJSON *trend = cJSON_AddArrayToObject(out, "trend");
    for (int c = 0; c < category_count; c++)
    {
        cJSON_AddItemToArray(categories, cJSON_CreateString(names[c]));
        bl_cents_t now, average;
        int pct = bl_category_trend(book, c, today, &now, &average);
        if (now > 0 || average > 0)
        {
            cJSON *j = cJSON_CreateObject();
            cJSON_AddNumberToObject(j, "category", c);
            add_money(j, "now", now);
            add_money(j, "average", average);
            cJSON_AddNumberToObject(j, "pct", pct);
            cJSON_AddItemToArray(trend, j);
        }
    }
    /* Recent movements for the overview. */
    cJSON *recent = cJSON_AddArrayToObject(out, "recent");
    for (int i = book->tx_count - 1, n = 0; i >= 0 && n < 8; i--)
    {
        if (book->tx[i].date <= today)
        {
            cJSON_AddItemToArray(recent, tx_json(&book->tx[i]));
            n++;
        }
    }
    cJSON *counts = cJSON_AddObjectToObject(out, "counts");
    cJSON_AddNumberToObject(counts, "tx", book->tx_count);
    cJSON_AddNumberToObject(counts, "tx_max", BL_MAX_TX);
    cJSON_AddNumberToObject(counts, "rules", book->rule_count);
    cJSON_AddNumberToObject(counts, "rules_max", BL_MAX_RULES);
    bl_unlock();
    free(items);
    return appkit_json(request, out);
}

/* Every rule with the movements that paid it (a bill's real amounts), newest first. */
static esp_err_t rules(httpd_req_t *request)
{
    GUARD();
    cJSON *out = cJSON_CreateArray();
    bl_lock();
    const bl_book_t *book = bl_book();
    for (int i = 0; i < book->rule_count; i++)
    {
        const bl_rule_t *r = &book->rules[i];
        cJSON *j = rule_json(r);
        cJSON *paid = cJSON_AddArrayToObject(j, "paid");
        for (int k = book->tx_count - 1, n = 0; k >= 0 && n < RULE_HISTORY; k--)
        {
            const bl_tx_t *t = &book->tx[k];
            if (t->rule == r->id)
            {
                cJSON *p = cJSON_CreateObject();
                add_date(p, "date", t->date);
                add_money(p, "amount", t->amount < 0 ? -t->amount : t->amount);
                cJSON_AddItemToArray(paid, p);
                n++;
            }
        }
        cJSON_AddItemToArray(out, j);
    }
    bl_unlock();
    return appkit_json(request, out);
}

static int newest_first(const void *a, const void *b)
{
    const bl_tx_t *x = *(const bl_tx_t *const *)a, *y = *(const bl_tx_t *const *)b;
    return x->date != y->date ? (x->date > y->date ? -1 : 1) : (x->id > y->id ? -1 : 1);
}

static esp_err_t tx_list(httpd_req_t *request)
{
    GUARD();
    char value[12];
    appkit_query(request, "month", value, sizeof(value));
    int month = bl_parse_month(value);
    bl_day_t today = bl_today();
    if (month == 0)
    {
        month = today != BL_NONE ? bl_month_of(today) : 0;
    }
    bl_day_t from = bl_month_start(month), to = bl_month_start(month + 1);
    const bl_tx_t **list = heap_caps_malloc(sizeof(bl_tx_t *) * BL_MAX_TX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (list == NULL)
    {
        return appkit_json(request, NULL);
    }
    cJSON *out = cJSON_CreateArray();
    bl_lock();
    const bl_book_t *book = bl_book();
    int n = 0;
    for (int i = 0; i < book->tx_count; i++)
    {
        if (book->tx[i].date >= from && book->tx[i].date < to)
        {
            list[n++] = &book->tx[i];
        }
    }
    qsort(list, (size_t)n, sizeof(list[0]), newest_first);
    for (int i = 0; i < n; i++)
    {
        cJSON_AddItemToArray(out, tx_json(list[i]));
    }
    bl_unlock();
    free(list);
    return appkit_json(request, out);
}

static esp_err_t months(httpd_req_t *request)
{
    GUARD();
    bl_day_t today = bl_today();
    if (today == BL_NONE)
    {
        return appkit_json(request, cJSON_CreateArray());
    }
    int now = bl_month_of(today);
    bl_cents_t in[BL_KEEP_MONTHS] = {0}, spent[BL_KEEP_MONTHS] = {0};
    int count[BL_KEEP_MONTHS] = {0};
    cJSON *out = cJSON_CreateArray();
    bl_lock();
    const bl_book_t *book = bl_book();
    for (int i = 0; i < book->tx_count; i++)
    {
        int k = now - bl_month_of(book->tx[i].date);
        if (k >= 0 && k < BL_KEEP_MONTHS && book->tx[i].date <= today)
        {
            if (book->tx[i].amount > 0)
            {
                in[k] += book->tx[i].amount;
            }
            else
            {
                spent[k] -= book->tx[i].amount;
            }
            count[k]++;
        }
    }
    int archived;
    const bl_archive_t *archive = bl_archive(&archived);
    for (int i = 0; i < archived; i++)
    {
        cJSON *j = cJSON_CreateObject();
        add_month(j, "month", archive[i].month);
        add_money(j, "in", archive[i].in);
        add_money(j, "out", archive[i].out);
        cJSON_AddNumberToObject(j, "count", -1);
        cJSON_AddItemToArray(out, j);
    }
    bl_unlock();
    for (int k = BL_KEEP_MONTHS - 1; k >= 0; k--)
    {
        cJSON *j = cJSON_CreateObject();
        add_month(j, "month", now - k);
        add_money(j, "in", in[k]);
        add_money(j, "out", spent[k]);
        cJSON_AddNumberToObject(j, "count", count[k]);
        cJSON_AddItemToArray(out, j);
    }
    return appkit_json(request, out);
}

static esp_err_t export(httpd_req_t *request)
{
    GUARD();
    char *data = bl_export();
    if (data == NULL)
    {
        return appkit_error(request, "503 Service Unavailable", "Memoria insufficiente");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_send(request, data, HTTPD_RESP_USE_STRLEN);
    free(data);
    return result;
}

/* ---------------------------------------------------------------- edits */

#define MAX_CENTS 100000000000LL   /* a billion euros: anything above is a typo */

static esp_err_t tx_post(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return bad(request, "Richiesta non valida");
    }
    bl_tx_t t = {0};
    t.id = (uint32_t)num(root, "id", 0);
    t.date = bl_parse_date(text(root, "date"));
    t.amount = (bl_cents_t)num(root, "amount", 0);
    t.category = (uint8_t)num(root, "category", 0);
    t.rule = (uint32_t)num(root, "rule", 0);
    bl_copy(t.note, sizeof(t.note), text(root, "note"));
    cJSON_Delete(root);
    if (t.date == BL_NONE)
    {
        return bad(request, "Data non valida");
    }
    if (t.amount == 0 || t.amount > MAX_CENTS || t.amount < -MAX_CENTS)
    {
        return bad(request, "Importo non valido");
    }
    uint32_t id = bl_tx_put(&t);
    return id != 0 ? ok_id(request, id) : bad(request, "Movimento non salvato (memoria piena o non trovato)");
}

static esp_err_t tx_delete(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    bool done = bl_tx_delete((uint32_t)num(root, "id", 0));
    cJSON_Delete(root);
    return done ? ok_id(request, 0) : bad(request, "Movimento non trovato");
}

static esp_err_t rule_post(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    const char *name = text(root, "name");
    if (!cJSON_IsObject(root) || name == NULL || name[0] == '\0')
    {
        cJSON_Delete(root);
        return bad(request, "Serve un nome");
    }
    bl_rule_t r = {0};
    r.id = (uint32_t)num(root, "id", 0);
    r.kind = num(root, "kind", BL_EXPENSE) == BL_INCOME ? BL_INCOME : BL_EXPENSE;
    bl_copy(r.name, sizeof(r.name), name);
    r.amount = (bl_cents_t)num(root, "amount", 0);
    int every = (int)num(root, "every", 1);
    r.every = (uint8_t)(bl_every_valid(every) ? every : 1);
    r.month = (uint8_t)num(root, "month", 1);
    r.day = (uint8_t)num(root, "day", 1);
    int shift = (int)num(root, "shift", r.kind == BL_INCOME ? BL_SHIFT_BEFORE : BL_SHIFT_EXACT);
    r.shift = (uint8_t)(shift >= BL_SHIFT_EXACT && shift <= BL_SHIFT_AFTER ? shift : BL_SHIFT_EXACT);
    r.category = (uint8_t)num(root, "category", 0);
    r.first = bl_parse_month(text(root, "first"));
    r.last = bl_parse_month(text(root, "last"));
    cJSON_Delete(root);
    if (r.amount <= 0 || r.amount > MAX_CENTS)
    {
        return bad(request, "Importo non valido");
    }
    if (r.day < 1 || r.day > 31 || r.month < 1 || r.month > 12)
    {
        return bad(request, "Giorno o mese non valido");
    }
    if (r.first != 0 && r.last != 0 && r.last < r.first)
    {
        return bad(request, "La fine viene prima dell'inizio");
    }
    if (r.every > 12 && r.first == 0)
    {
        return bad(request, "Indica il mese della prossima scadenza");
    }
    uint32_t id = bl_rule_put(&r);
    return id != 0 ? ok_id(request, id) : bad(request, "Non salvato (massimo 64 voci fisse)");
}

static esp_err_t rule_delete(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    bool done = bl_rule_delete((uint32_t)num(root, "id", 0));
    cJSON_Delete(root);
    return done ? ok_id(request, 0) : bad(request, "Voce non trovata");
}

static esp_err_t balance_post(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    bl_day_t date = bl_parse_date(text(root, "date"));
    const cJSON *amount = cJSON_GetObjectItemCaseSensitive(root, "amount");
    bool valid = cJSON_IsNumber(amount) && amount->valuedouble < MAX_CENTS && amount->valuedouble > -MAX_CENTS;
    bl_cents_t cents = valid ? (bl_cents_t)amount->valuedouble : 0;
    cJSON_Delete(root);
    bl_day_t today = bl_today();
    if (!valid || date == BL_NONE || (today != BL_NONE && date > today))
    {
        return bad(request, "Saldo o data non validi (la data non può essere nel futuro)");
    }
    bl_set_balance(cents, date);
    return ok_id(request, 0);
}

static esp_err_t category_post(httpd_req_t *request)
{
    GUARD();
    cJSON *root = body_json(request);
    int index = (int)num(root, "index", -1);
    const char *name = text(root, "name");
    bool done = name != NULL && (index < 0 ? bl_category_add(name) >= 0 : bl_category_rename(index, name));
    cJSON_Delete(root);
    return done ? ok_id(request, 0) : bad(request, "Categoria non salvata (nome vuoto, doppio o massimo 16)");
}

static esp_err_t import(httpd_req_t *request)
{
    GUARD();
    char *raw = appkit_body(request, 1024 * 1024);
    if (raw == NULL)
    {
        return bad(request, "File vuoto o troppo grande");
    }
    char why[64];
    bool done = bl_import(raw, why, sizeof(why));
    free(raw);
    return done ? ok_id(request, 0) : bad(request, why);
}

static const httpd_uri_t s_routes[] = {
    {.uri = "/api/state", .method = HTTP_GET, .handler = state},
    {.uri = "/api/rules", .method = HTTP_GET, .handler = rules},
    {.uri = "/api/tx", .method = HTTP_GET, .handler = tx_list},
    {.uri = "/api/months", .method = HTTP_GET, .handler = months},
    {.uri = "/api/export", .method = HTTP_GET, .handler = export},
    {.uri = "/api/tx", .method = HTTP_POST, .handler = tx_post},
    {.uri = "/api/tx/delete", .method = HTTP_POST, .handler = tx_delete},
    {.uri = "/api/rule", .method = HTTP_POST, .handler = rule_post},
    {.uri = "/api/rule/delete", .method = HTTP_POST, .handler = rule_delete},
    {.uri = "/api/balance", .method = HTTP_POST, .handler = balance_post},
    {.uri = "/api/category", .method = HTTP_POST, .handler = category_post},
    {.uri = "/api/import", .method = HTTP_POST, .handler = import},
};

static const appkit_web_config_t s_config = {
    .name = "Bilancio",
    .page_start = dashboard_html_start,
    .page_end = dashboard_html_end,
    .routes = s_routes,
    .route_count = sizeof(s_routes) / sizeof(s_routes[0]),
};

esp_err_t bilancio_web_start(void)
{
    esp_err_t err = bl_start();
    return err == ESP_OK ? appkit_web_start(&s_config) : err;
}
