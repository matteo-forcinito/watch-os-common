/*
 * Bilancio on the watch (the look of Copertura's LIVE screen).
 *
 *   HOME    outer arc = what is left of this period's money (red below zero),
 *           inner arc = today's budget still free; the balance in big digits,
 *           today, days to payday, forecast; round buttons.
 *   MONTH   this month's fixed costs and income, ✓ when passed or paid.
 *   ADD     quick expense: 5/10/20/50 € -> a category -> recorded today.
 *   SUMMARY available now, free until payday, income and expenses per week,
 *           month and year, monthly saving, year-end estimate.
 *   MENU / DASH (QR code and pairing of the dashboard).
 */
#include "bilancio.h"

#include "appkit.h"
#include "core/lv_port.h"
#include "core/power.h"
#if !CONFIG_APPKIT_HOSTED
#include "extapp_sdk.h"
#endif
#include "hardware/board.h"
#include "ui/ui.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_BG     0x060B12
#define C_CARD   0x111A26
#define C_LINE   0x1F2C3D
#define C_INK    0xEEF4FB
#define C_DIM    0x8B9AB0
#define C_ACCENT 0x14B87A

enum { HOME, MONTH, ADD, ADD_CATEGORY, MENU, DASH, SUMMARY };

typedef struct
{
    lv_obj_t *screen;
    lv_obj_t *content;
    lv_timer_t *timer;
    int view;
    uint32_t revision;
    bl_day_t day;
    bl_cents_t pending;           /* quick expense waiting for its category */
} bl_ui_t;

static bl_ui_t *s_ui;
static void render(bl_ui_t *ui);

static const uint32_t STATE_COLOR[] = {0x2EE39A, 0xFFA34D, 0xFF6B6B};
static const char *const STATE_WORD[] = {"in linea", "sopra il ritmo", "attenzione"};

/* 123456 cents -> "1.234,56" (cents dropped when `whole`). */
static void money(char *out, size_t size, bl_cents_t cents, bool whole)
{
    bool negative = cents < 0;
    unsigned long long a = (unsigned long long)(negative ? -cents : cents);
    unsigned long long euros = a / 100;
    char digits[24], grouped[32];
    snprintf(digits, sizeof(digits), "%llu", euros);
    size_t n = strlen(digits), g = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (i > 0 && (n - i) % 3 == 0)
        {
            grouped[g++] = '.';
        }
        grouped[g++] = digits[i];
    }
    grouped[g] = '\0';
    if (whole)
    {
        snprintf(out, size, "%s%s", negative ? "-" : "", grouped);
    }
    else
    {
        snprintf(out, size, "%s%s,%02llu", negative ? "-" : "", grouped, a % 100);
    }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_color(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static void go(bl_ui_t *ui, int view)
{
    ui->view = view;
    render(ui);
}

static void open_view(lv_event_t *e)
{
    go(s_ui, (int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, uint32_t color, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 56, 56);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, ui_color(C_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, ui_color(C_LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_bg_color(b, ui_color(C_LINE), LV_STATE_PRESSED);
    lv_obj_center(label(b, UI_FONT_LARGE, color, symbol));
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_port_set_focusable(b);
    return b;
}

/* A 270° arc, value 0..100, nothing clickable. */
static lv_obj_t *gauge(lv_obj_t *parent, int size, int width, uint32_t color, int value)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, ui_color(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, ui_color(color), LV_PART_INDICATOR);
    lv_arc_set_value(arc, value < 0 ? 0 : value > 100 ? 100 : value);
    lv_obj_set_clickable(arc, false);
    return arc;
}

/* ------------------------------------------------------------------ HOME */

static void home(bl_ui_t *ui)
{
    const board_info_t *b = board_info();
    bl_day_t today = bl_today();
    if (today == BL_NONE)
    {
        ui_text(ui_page(ui->content, "Bilancio"), "L'orologio non ha ancora l'ora: collegalo al Wi-Fi.", true);
        return;
    }
    bl_summary_t s;
    bl_lock();
    bl_summary(bl_book(), today, &s);
    bool any_rule = bl_book()->rule_count > 0;
    bl_unlock();

    int size = b->round ? (b->width < b->height ? b->width : b->height) - 20 : b->width - 70;
    /* Outer arc: what is left of the money of this period (balance / balance + spent since payday). */
    bl_cents_t start_money = s.balance + s.period_spent;
    int left_pct = s.balance > 0 && start_money > 0 ? (int)(s.balance * 100 / start_money) : 0;
    uint32_t outer_color = s.balance < 0 ? 0xFF6B6B : STATE_COLOR[s.state];
    lv_obj_t *arc = gauge(ui->content, size, size / 16, outer_color, s.balance < 0 ? 100 : left_pct);
    lv_obj_align(arc, b->round ? LV_ALIGN_CENTER : LV_ALIGN_TOP_MID, 0, b->round ? 0 : 6);
    /* Inner arc: today's budget still free. */
    int inner = size * 76 / 100;
    int today_pct = s.daily > 0 && s.today_left > 0 ? (int)(s.today_left * 100 / s.daily) : 0;
    lv_obj_t *small = gauge(ui->content, inner, size / 30, s.today_left > 0 ? 0x4DA3FF : 0xFF6B6B, today_pct);
    lv_obj_align_to(small, arc, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *col = box(ui->content);
    lv_obj_set_size(col, inner * 74 / 100, LV_SIZE_CONTENT);
    lv_obj_align_to(col, arc, LV_ALIGN_CENTER, 0, -size / 16);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 1, 0);
    char text[48], amount[24];
    if (!any_rule && !s.has_balance)
    {
        lv_obj_t *t = label(col, UI_FONT_BODY, C_INK, "Aggiungi entrate e spese");
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(t, inner * 70 / 100);
        lv_obj_t *d = label(col, UI_FONT_SMALL, C_DIM, "dalla Dashboard (menu " LV_SYMBOL_SETTINGS ")");
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    }
    else
    {
        label(col, UI_FONT_SMALL, C_DIM, "disponibili");
        money(amount, sizeof(amount), s.balance, true);
        snprintf(text, sizeof(text), "%s €", amount);
        label(col, ui_font_big, s.balance < 0 ? 0xFF6B6B : C_INK, text);
        money(amount, sizeof(amount), s.today_left > 0 ? s.today_left : 0, true);
        snprintf(text, sizeof(text), "oggi %s € · %s", amount, STATE_WORD[s.state]);
        label(col, UI_FONT_SMALL, s.today_left > 0 ? 0x4DA3FF : 0xFF6B6B, text);
        if (s.main_income >= 0)
        {
            snprintf(text, sizeof(text), s.days == 1 ? "stipendio domani" : "stipendio tra %d giorni", s.days);
        }
        else
        {
            snprintf(text, sizeof(text), "fine mese tra %d giorni", s.days);
        }
        label(col, UI_FONT_SMALL, C_DIM, text);
        money(amount, sizeof(amount), s.forecast, true);
        snprintf(text, sizeof(text), "previsione %s €", amount);
        label(col, UI_FONT_SMALL, s.forecast < 0 ? 0xFF6B6B : 0x5C6B80, text);
    }

    lv_obj_t *buttons = box(ui->content);
    lv_obj_set_size(buttons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 10, 0);
    round_button(buttons, LV_SYMBOL_PLUS, 0x2EE39A, open_view, (void *)(intptr_t)ADD);
    round_button(buttons, LV_SYMBOL_LIST, C_ACCENT, open_view, (void *)(intptr_t)MONTH);
    round_button(buttons, LV_SYMBOL_SETTINGS, C_DIM, open_view, (void *)(intptr_t)MENU);
    lv_obj_align(buttons, LV_ALIGN_BOTTOM_MID, 0, b->round ? -10 : -10);
}

/* ----------------------------------------------------------------- MONTH */

typedef struct
{
    lv_obj_t *page;
    const bl_book_t *book;
    bl_day_t today;
    int shown;
} month_ctx_t;

static void month_row(const bl_rule_t *r, bl_day_t date, void *data)
{
    month_ctx_t *m = data;
    if (m->shown >= 24)
    {
        return;
    }
    int y, mo, d;
    bl_civil(date, &y, &mo, &d);
    bool done = bl_paid(m->book, r, date) || date < m->today || (r->kind == BL_INCOME && date == m->today);
    char title[48], value[24], amount[20];
    snprintf(title, sizeof(title), "%d/%d  %s", d, mo, r->name);
    money(amount, sizeof(amount), r->amount, false);
    snprintf(value, sizeof(value), "%s%s", r->kind == BL_INCOME ? "+" : "", amount);
    ui_row(m->page, done ? LV_SYMBOL_OK : LV_SYMBOL_BELL, done ? 0x2EE39A : (r->kind == BL_INCOME ? 0x2EE39A : 0xFFA34D),
           title, value, NULL, NULL);
    m->shown++;
}

static void month(bl_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Questo mese");
    bl_day_t today = bl_today();
    if (today == BL_NONE)
    {
        ui_text(page, "Ora non impostata.", true);
        return;
    }
    int mi = bl_month_of(today);
    month_ctx_t m = {.page = page, .today = today};
    char text[64], amount[24];
    bl_lock();
    m.book = bl_book();
    bl_each_date(m.book->rules, m.book->rule_count, bl_month_start(mi), bl_month_start(mi + 1), month_row, &m);
    bl_cents_t fixed = bl_month_total(m.book->rules, m.book->rule_count, BL_EXPENSE, mi);
    bl_cents_t aside = bl_set_aside(m.book->rules, m.book->rule_count, mi);
    int ratio = bl_fixed_ratio(m.book->rules, m.book->rule_count, mi);
    bl_unlock();
    if (m.shown == 0)
    {
        ui_text(page, "Nessuna scadenza: aggiungi i fissi dalla dashboard.", true);
        return;
    }
    money(amount, sizeof(amount), fixed, false);
    snprintf(text, sizeof(text), "Fisso del mese: %s €", amount);
    ui_text(page, text, false);
    if (ratio >= 0)
    {
        snprintf(text, sizeof(text), "Il fisso pesa il %d%% delle entrate", ratio);
        ui_text(page, text, true);
    }
    if (aside > 0)
    {
        money(amount, sizeof(amount), aside, false);
        snprintf(text, sizeof(text), "Da mettere da parte: %s €/mese", amount);
        ui_text(page, text, true);
    }
}

/* ------------------------------------------------------------------- ADD */

static void pick_amount(lv_event_t *e)
{
    s_ui->pending = (bl_cents_t)(intptr_t)lv_event_get_user_data(e) * 100;
    go(s_ui, ADD_CATEGORY);
}

static void add(bl_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Spesa veloce");
    static const int amounts[] = {5, 10, 20, 50};
    for (size_t i = 0; i < sizeof(amounts) / sizeof(amounts[0]); i++)
    {
        char text[16];
        snprintf(text, sizeof(text), "%d €", amounts[i]);
        lv_obj_t *b = ui_button(page, text, i == 0 ? C_ACCENT : 0x2A3A50, pick_amount, (void *)(intptr_t)amounts[i]);
        lv_port_set_focusable(b);
    }
    ui_text(page, "Importi diversi, note e modifiche dalla dashboard.", true);
}

static void pick_category(lv_event_t *e)
{
    bl_ui_t *ui = s_ui;
    bl_day_t today = bl_today();
    bl_tx_t t = {.date = today, .amount = -ui->pending, .category = (uint8_t)(intptr_t)lv_event_get_user_data(e)};
    if (today != BL_NONE && bl_tx_put(&t) != 0)
    {
        ui->pending = 0;
        go(ui, HOME);
    }
}

static void add_category(bl_ui_t *ui)
{
    char title[32], amount[16];
    money(amount, sizeof(amount), ui->pending, true);
    snprintf(title, sizeof(title), "%s € per...", amount);
    lv_obj_t *page = ui_page(ui->content, title);
    int count;
    char names[BL_MAX_CATEGORIES][BL_CATEGORY_LEN];
    bl_lock();
    const char (*list)[BL_CATEGORY_LEN] = bl_categories(&count);
    memcpy(names, list, sizeof(names[0]) * (size_t)count);
    bl_unlock();
    static const uint32_t colors[] = {0x3987E5, 0xD95926, 0x199E70, 0xC98500, 0xD55181, 0x2FA84F, 0x9085E9, 0xE66767};
    for (int i = 0; i < count; i++)
    {
        lv_obj_t *row = ui_row(page, LV_SYMBOL_RIGHT, i < 8 ? colors[i] : C_DIM, names[i], "", pick_category, (void *)(intptr_t)i);
        lv_port_set_focusable(row);
    }
}

/* ------------------------------------------------------------ MENU, DASH */

/* One "label ...... 1.234 €" row, red when negative if `sign`. */
static void figure(lv_obj_t *page, const char *icon, uint32_t color, const char *text, bl_cents_t cents, bool sign)
{
    char amount[24], value[32];
    money(amount, sizeof(amount), cents, true);
    snprintf(value, sizeof(value), "%s €", amount);
    lv_obj_t *row = ui_row(page, icon, sign && cents < 0 ? 0xFF6B6B : color, text, value, NULL, NULL);
    if (sign && cents < 0)
    {
        lv_obj_set_style_text_color(ui_row_value(row), ui_color(0xFF6B6B), 0);
    }
}

static void summary(bl_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Riepilogo");
    bl_day_t today = bl_today();
    if (today == BL_NONE)
    {
        ui_text(page, "Ora non impostata.", true);
        return;
    }
    int mi = bl_month_of(today);
    bl_summary_t s;
    bl_lock();
    const bl_book_t *book = bl_book();
    bl_summary(book, today, &s);
    bl_cents_t income = bl_monthly_share(book->rules, book->rule_count, BL_INCOME, mi);
    bl_cents_t fixed = bl_monthly_share(book->rules, book->rule_count, BL_EXPENSE, mi);
    bl_cents_t yearly = bl_yearly_cost(book->rules, book->rule_count, BL_ANY_CATEGORY, mi);
    bool balance_set = book->balance_date != BL_NONE;
    int ncat = 0, loans_category = -1;
    const char (*names)[BL_CATEGORY_LEN] = bl_categories(&ncat);
    for (int i = 0; i < ncat; i++)
    {
        loans_category = strcmp(names[i], "rate") == 0 ? i : loans_category;
    }
    bl_month_end_t me;
    bl_month_end(book, today, loans_category, &me);
    bl_unlock();
    bl_cents_t variable = s.pace * 365 / 12, out = fixed + variable, net = income - out;
    int year, month_number, day;
    bl_civil(today, &year, &month_number, &day);

    ui_section(page, "Adesso");
    figure(page, LV_SYMBOL_HOME, 0x4DA3FF, "Disponibile", s.balance, true);
    figure(page, LV_SYMBOL_OK, 0x2EE39A, "Liberi fino allo stipendio", s.free, true);
    figure(page, LV_SYMBOL_BELL, 0xFFA34D, "Ancora da pagare", s.due, false);
    figure(page, LV_SYMBOL_RIGHT, 0x2EE39A, "Al giorno", s.daily, false);
    figure(page, LV_SYMBOL_EYE_OPEN, 0x8E7DFF, "Previsione allo stipendio", s.forecast, true);
    char text[80];
    snprintf(text, sizeof(text), "%s. Stipendio fra %d %s.", balance_set ? "Saldo dalla banca" : "Saldo calcolato da entrate e uscite",
             s.days, s.days == 1 ? "giorno" : "giorni");
    ui_text(page, text, true);

    ui_section(page, "A fine mese");
    figure(page, LV_SYMBOL_DOWNLOAD, 0x2EE39A, "Entrate in arrivo", me.income, false);
    figure(page, LV_SYMBOL_UPLOAD, 0xFFA34D, "Spese fisse da pagare", me.fixed, false);
    figure(page, LV_SYMBOL_SAVE, 0x4DA3FF, "Dopo rate e mutui", me.after_loans, true);
    figure(page, LV_SYMBOL_SAVE, 0x8E7DFF, "Dopo tutte le spese", me.after_all, true);

    ui_section(page, "Al mese");
    figure(page, LV_SYMBOL_DOWNLOAD, 0x2EE39A, "Entrate", income, false);
    figure(page, LV_SYMBOL_UPLOAD, 0xFFA34D, "Uscite fisse", fixed, false);
    figure(page, LV_SYMBOL_UPLOAD, 0xFF8A65, "Uscite variabili", variable, false);
    figure(page, LV_SYMBOL_SAVE, 0x4DA3FF, "Risparmio", net, true);

    ui_section(page, "Alla settimana");
    figure(page, LV_SYMBOL_DOWNLOAD, 0x2EE39A, "Entrate", income * 12 / 52, false);
    figure(page, LV_SYMBOL_UPLOAD, 0xFFA34D, "Uscite", out * 12 / 52, false);

    ui_section(page, "All'anno");
    figure(page, LV_SYMBOL_DOWNLOAD, 0x2EE39A, "Entrate", income * 12, false);
    figure(page, LV_SYMBOL_UPLOAD, 0xFFA34D, "Uscite", out * 12, false);
    figure(page, LV_SYMBOL_LIST, 0xFFA34D, "di cui fisse", yearly, false);
    figure(page, LV_SYMBOL_SAVE, 0x4DA3FF, "Risparmio", net * 12, true);
    figure(page, LV_SYMBOL_EYE_OPEN, 0x8E7DFF, "Stima a fine anno", s.balance + net * (12 - month_number), true);
    ui_text(page, "Fissi in quota mensile, spese variabili al ritmo degli ultimi 30 giorni.", true);
}

static void menu(bl_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Bilancio");
    lv_port_set_focusable(ui_row(page, LV_SYMBOL_LIST, 0x4DA3FF, "Riepilogo", NULL, open_view, (void *)(intptr_t)SUMMARY));
    lv_port_set_focusable(ui_row(page, LV_SYMBOL_BELL, 0xFFA34D, "Questo mese", NULL, open_view, (void *)(intptr_t)MONTH));
    lv_obj_t *b = ui_button(page, LV_SYMBOL_WIFI "  Dashboard", C_ACCENT, open_view, (void *)(intptr_t)DASH);
    lv_port_set_focusable(b);
    ui_text(page, "Saldo, stipendio, mutuo, bollette e movimenti si inseriscono dalla dashboard sul telefono "
                  "o sul PC: inquadra il codice QR.", true);
    ui_text(page, "I consigli sono regole semplici, non consulenza finanziaria. I dati restano sull'orologio.", true);
}

static void dash(bl_ui_t *ui)
{
    appkit_dashboard_page(ui_page(ui->content, "Dashboard"), ui_color(C_ACCENT));
}

/* ----------------------------------------------------------------- frame */

static void render(bl_ui_t *ui)
{
    if (ui->content != NULL)
    {
        lv_obj_delete(ui->content);
    }
    ui->content = box(ui->screen);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->content, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->content, LV_OPA_COVER, 0);
    lv_obj_move_to_index(ui->content, 0);
    ui->revision = bl_revision();
    ui->day = bl_today();
    switch (ui->view)
    {
    case HOME: home(ui); break;
    case MONTH: month(ui); break;
    case ADD: add(ui); break;
    case ADD_CATEGORY: add_category(ui); break;
    case MENU: menu(ui); break;
    case SUMMARY: summary(ui); break;
    default: dash(ui); break;
    }
}

/* Redraw when the dashboard changed something or the day turned. */
static void tick(lv_timer_t *timer)
{
    bl_ui_t *ui = lv_timer_get_user_data(timer);
    if (power_screen() == POWER_SCREEN_OFF || lv_screen_active() != ui->screen)
    {
        return;
    }
    if ((ui->view == HOME || ui->view == MONTH) && (ui->revision != bl_revision() || ui->day != bl_today()))
    {
        render(ui);
    }
}

bool bilancio_ui_back(void)
{
    bl_ui_t *ui = s_ui;
    if (ui == NULL)
    {
        return false;
    }
    if (ui->view == HOME)
    {
#if CONFIG_APPKIT_HOSTED
        return false;   /* internal app: the app manager closes it */
#else
        extapp_return_to_launcher();
#endif
    }
    else
    {
        go(ui, ui->view == ADD_CATEGORY ? ADD : ui->view == DASH ? MENU : HOME);
    }
    return true;
}

void bilancio_ui_create(lv_obj_t *screen)
{
    bl_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui = ui;
    ui->screen = screen;
    lv_obj_set_style_bg_color(screen, ui_color(C_BG), 0);
    render(ui);
    ui->timer = lv_timer_create(tick, 1000, ui);
}

void bilancio_ui_destroy(void)
{
    bl_ui_t *ui = s_ui;
    if (ui == NULL)
    {
        return;
    }
    s_ui = NULL;
    if (ui->timer != NULL)
    {
        lv_timer_delete(ui->timer);
    }
    free(ui);
}
