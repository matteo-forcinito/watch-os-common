/*
 * News on the watch.
 *
 *   FRONT     the front page: today's biggest stories (told by most sources)
 *   SECTIONS  the paper's sections with their article counts
 *   SECTION   one section's headlines, newest first
 *   STORY     an article: summary, the same story from other sources,
 *             "Contesto" (Wikipedia) and "Cronologia" (the archive)
 *   CONTEXT   Wikipedia's summary for the story's keywords
 *   TIMELINE  archive headlines matching the keywords, newest first
 *   TRENDS    the words of the day / week / month / year
 *   MENU      search, refresh, background collection, dashboard
 *   DASH      QR code + code for the web dashboard (appkit)
 *
 * The UI reads the data under nw_lock; the job task does the work.
 */
#include "news.h"

#include "appkit.h"
#include "core/lv_port.h"
#include "hardware/board.h"
#include "ui/ui.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define C_BG      0x0B0B0D
#define C_CARD    0x18181C
#define C_LEAD    0x23201C
#define C_LINE    0x2C2C32
#define C_INK     0xF4F1EA
#define C_DIM     0x9A968E
#define C_ACCENT  0xFF6B5B
#define C_GOOD    0x30D158

#define FRONT_MAX   14
#define LIST_MAX    40
#define HISTORY     10

enum { FRONT, SECTIONS, SECTION, STORY, CONTEXT, TIMELINE, TRENDS, MENU, DASH };

typedef struct
{
    lv_obj_t *screen;
    lv_obj_t *content;
    lv_obj_t *status;            /* status line on FRONT (updated in place) */
    lv_obj_t *border;
    lv_timer_t *timer;
    int view;
    int history[HISTORY];
    int depth;
    int topic;
    uint32_t item;               /* STORY: item id */
    int period;
    char query[80];              /* CONTEXT / TIMELINE */
    nw_phase_t phase;
    uint32_t revision;
} news_ui_t;

static news_ui_t *s_ui;
static void render(news_ui_t *ui);

/* ------------------------------------------------------------- helpers */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_color(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static int card_width(void)
{
    const board_info_t *b = board_info();
    return b->round ? b->width * 74 / 100 : b->width - 24;
}

static void go(news_ui_t *ui, int view)
{
    if (ui->depth < HISTORY)
    {
        ui->history[ui->depth++] = ui->view;
    }
    ui->view = view;
    render(ui);
}

static uint32_t topic_color(int topic)
{
    int count;
    const nw_topic_t *t = nw_topics(&count);
    return topic < count ? t[topic].color : C_DIM;
}

static void topic_name(int topic, char *out, size_t size)
{
    int count;
    const nw_topic_t *t = nw_topics(&count);
    snprintf(out, size, "%s", topic < count ? t[topic].name : "");
}

static void feed_name(int feed, char *out, size_t size)
{
    int count;
    const nw_feed_t *f = nw_feeds(&count);
    snprintf(out, size, "%s", feed < count ? f[feed].name : "");
}

/* A headline card: colored edge for the section, title, one meta line. */
static lv_obj_t *headline(lv_obj_t *page, bool lead, uint32_t color, const char *title, const char *meta,
                          lv_event_cb_t cb, void *data)
{
    int width = card_width();
    lv_obj_t *card = lv_button_create(page);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, ui_color(lead ? C_LEAD : C_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, ui_color(C_LINE), LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_left(card, 18, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(card, 5, 0);
    lv_obj_set_style_border_color(card, ui_color(color), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    if (cb != NULL)
    {
        lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, data);
        lv_port_set_focusable(card);
    }
    lv_obj_t *t = label(card, lead ? ui_font_headline : UI_FONT_BODY, C_INK, title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, width - 30);
    if (meta != NULL && meta[0])
    {
        lv_obj_t *m = label(card, UI_FONT_SMALL, C_DIM, meta);
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(m, width - 30);
    }
    return card;
}

/* A row of small pill buttons (view switches). */
static lv_obj_t *pill_row(lv_obj_t *page)
{
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, card_width(), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 8, 0);
    return row;
}

static lv_obj_t *pill(lv_obj_t *row, const char *text, bool on, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_button_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_ver(b, 9, 0);
    lv_obj_set_style_radius(b, 99, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, ui_color(on ? C_ACCENT : C_CARD), 0);
    lv_obj_set_style_bg_color(b, ui_color(C_LINE), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_port_set_focusable(b);
    label(b, UI_FONT_SMALL, on ? 0x1A0A08 : C_INK, text);
    return b;
}

/* The story's two strongest keywords: "Iran Teheran". */
static void story_query(const nw_item_t *it, char *out, size_t size)
{
    nw_key_t keys[NW_KEY_MAX];
    int n = nw_keywords(it->title, keys, NW_KEY_MAX);
    out[0] = '\0';
    for (int i = 0; i < n && i < 2; i++)
    {
        size_t len = strlen(out);
        snprintf(out + len, size - len, "%s%s", i ? " " : "", keys[i].word);
    }
}

/* -------------------------------------------------------------- events */

static void open_item(lv_event_t *e)
{
    s_ui->item = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    go(s_ui, STORY);
}

static void open_view(lv_event_t *e)
{
    go(s_ui, (int)(intptr_t)lv_event_get_user_data(e));
}

static void open_topic(lv_event_t *e)
{
    s_ui->topic = (int)(intptr_t)lv_event_get_user_data(e);
    go(s_ui, SECTION);
}

static void set_period(lv_event_t *e)
{
    s_ui->period = (int)(intptr_t)lv_event_get_user_data(e);
    render(s_ui);
}

static void open_term(lv_event_t *e)
{
    const char *word = lv_label_get_text(lv_obj_get_child(lv_event_get_current_target(e), 0));
    snprintf(s_ui->query, sizeof(s_ui->query), "%s", word);
    nw_search(s_ui->query, 3);
    go(s_ui, TIMELINE);
}

static void ask_context(lv_event_t *e)
{
    (void)e;
    nw_context(s_ui->query);
    go(s_ui, CONTEXT);
}

static void ask_timeline(lv_event_t *e)
{
    (void)e;
    nw_search(s_ui->query, 3);
    go(s_ui, TIMELINE);
}

static void search_more(lv_event_t *e)
{
    (void)e;
    nw_search(s_ui->query, 12);
    render(s_ui);
}

static void search_entered(const char *text, void *user_data)
{
    (void)user_data;
    if (s_ui == NULL || text == NULL || text[0] == '\0')
    {
        return;
    }
    snprintf(s_ui->query, sizeof(s_ui->query), "%s", text);
    nw_search(s_ui->query, 3);
    go(s_ui, TIMELINE);
}

static void ask_search(lv_event_t *e)
{
    (void)e;
    ui_text_input("Cerca nelle notizie", "", false, search_entered, NULL);
}

static void refresh(lv_event_t *e)
{
    (void)e;
    nw_refresh();
    ui_toast("Aggiorno i giornali...");
}

static void cycle_background(lv_event_t *e)
{
    static const uint8_t steps[] = {0, 1, 2, 4, 8};
    nw_settings_t s;
    nw_settings(&s);
    int i = 0;
    while (i < 5 && steps[i] != s.every_hours)
    {
        i++;
    }
    s.every_hours = steps[(i + 1) % 5];
    nw_set_settings(&s);
    lv_obj_t *value = ui_row_value(lv_event_get_current_target(e));
    char text[24];
    if (s.every_hours)
    {
        snprintf(text, sizeof(text), "ogni %d h", s.every_hours);
    }
    else
    {
        snprintf(text, sizeof(text), "solo app aperta");
    }
    lv_label_set_text(value, text);
}

/* --------------------------------------------------------------- FRONT */

static void front(news_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, NULL);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    static const char *const days[] = {"domenica", "lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato"};
    static const char *const months[] = {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio",
                                         "agosto", "settembre", "ottobre", "novembre", "dicembre"};
    lv_obj_t *mast = label(page, ui_font_title, C_INK, "Il Quotidiano");
    lv_obj_set_style_text_align(mast, LV_TEXT_ALIGN_CENTER, 0);
    char text[160];
    if (now > 1700000000)
    {
        snprintf(text, sizeof(text), "%s %d %s", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon]);
        lv_obj_t *date = label(page, UI_FONT_SMALL, C_ACCENT, text);
        lv_obj_set_style_text_align(date, LV_TEXT_ALIGN_CENTER, 0);
    }
    nw_status_t st;
    nw_status(&st);
    ui->status = label(page, UI_FONT_SMALL, C_DIM, st.message);
    lv_obj_set_width(ui->status, card_width());
    lv_obj_set_style_text_align(ui->status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui->status, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *row = pill_row(page);
    pill(row, "Sezioni", false, open_view, (void *)(intptr_t)SECTIONS);
    pill(row, "Classifiche", false, open_view, (void *)(intptr_t)TRENDS);
    pill(row, LV_SYMBOL_BARS, false, open_view, (void *)(intptr_t)MENU);

    nw_lock();
    int story_count = 0, item_count = 0;
    const nw_story_t *stories = nw_stories(&story_count);
    const nw_item_t *items = nw_items(&item_count);
    int shown = 0;
    for (int s = 0; s < story_count && shown < FRONT_MAX; s++)
    {
        const nw_story_t *st2 = &stories[s];
        if (st2->lead < 0 || st2->lead >= item_count)
        {
            continue;
        }
        const nw_item_t *it = &items[st2->lead];
        char when[24], topic[28];
        nw_time_text(st2->last, when, sizeof(when));
        topic_name(it->topic, topic, sizeof(topic));
        if (st2->sources > 1)
        {
            snprintf(text, sizeof(text), "%s · %d fonti · %s", topic, st2->sources, when);
        }
        else
        {
            char source[28];
            feed_name(it->feed, source, sizeof(source));
            snprintf(text, sizeof(text), "%s · %s · %s", topic, source, when);
        }
        headline(page, shown == 0, topic_color(it->topic), it->title, text, open_item, (void *)(uintptr_t)it->id);
        shown++;
    }
    nw_unlock();
    if (shown == 0)
    {
        ui_text(page, "Le notizie arrivano al primo collegamento Wi-Fi: un minuto e ci siamo.", true);
    }
    else
    {
        ui_text(page, "In alto le storie raccontate da più giornali.", true);
    }
}

/* ------------------------------------------------------------ SECTIONS */

static void sections(news_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Sezioni");
    nw_lock();
    int topic_count, item_count;
    const nw_topic_t *topics = nw_topics(&topic_count);
    const nw_item_t *items = nw_items(&item_count);
    for (int t = 0; t < topic_count; t++)
    {
        if (!topics[t].enabled)
        {
            continue;
        }
        int n = 0;
        for (int i = 0; i < item_count; i++)
        {
            n += items[i].topic == t;
        }
        char count[12];
        snprintf(count, sizeof(count), "%d", n);
        ui_row(page, LV_SYMBOL_LIST, topics[t].color, topics[t].name, count, open_topic, (void *)(intptr_t)t);
    }
    nw_unlock();
    ui_text(page, "Sezioni e giornali si scelgono dalla dashboard.", true);
}

static void section(news_ui_t *ui)
{
    char name[28];
    nw_lock();
    topic_name(ui->topic, name, sizeof(name));
    nw_unlock();
    lv_obj_t *page = ui_page(ui->content, name);
    int16_t *list = malloc(sizeof(int16_t) * LIST_MAX);
    if (list == NULL)
    {
        return;
    }
    nw_lock();
    int n = nw_topic_items(ui->topic, list, LIST_MAX);
    int count;
    const nw_item_t *items = nw_items(&count);
    for (int i = 0; i < n; i++)
    {
        const nw_item_t *it = &items[list[i]];
        char when[24], source[28], meta[64];
        nw_time_text(it->when, when, sizeof(when));
        feed_name(it->feed, source, sizeof(source));
        snprintf(meta, sizeof(meta), "%s · %s", source, when);
        headline(page, false, topic_color(it->topic), it->title, meta, open_item, (void *)(uintptr_t)it->id);
    }
    nw_unlock();
    free(list);
    if (n == 0)
    {
        ui_text(page, "Ancora nessuna notizia in questa sezione.", true);
    }
}

/* --------------------------------------------------------------- STORY */

static void story(news_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, NULL);
    int16_t *others = malloc(sizeof(int16_t) * 12);
    nw_lock();
    int count;
    const nw_item_t *items = nw_items(&count);
    int index = -1;
    for (int i = 0; i < count; i++)
    {
        if (items[i].id == ui->item)
        {
            index = i;
            break;
        }
    }
    if (index < 0 || others == NULL)
    {
        nw_unlock();
        free(others);
        ui_text(page, "Questa notizia non è più in memoria.", true);
        return;
    }
    const nw_item_t *it = &items[index];
    char topic[28], source[28], when[24], meta[96];
    topic_name(it->topic, topic, sizeof(topic));
    feed_name(it->feed, source, sizeof(source));
    nw_time_text(it->when, when, sizeof(when));
    snprintf(meta, sizeof(meta), "%s · %s · %s", topic, source, when);
    lv_obj_t *m = label(page, UI_FONT_SMALL, topic_color(it->topic), meta);
    lv_obj_set_width(m, card_width());
    lv_obj_t *t = label(page, ui_font_headline, C_INK, it->title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, card_width());
    if (it->summary[0])
    {
        lv_obj_t *s = label(page, UI_FONT_BODY, 0xD9D4CA, it->summary);
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(s, card_width());
    }
    story_query(it, ui->query, sizeof(ui->query));
    int n = it->story >= 0 ? nw_story_items(it->story, others, 12) : 0;
    bool header = false;
    for (int i = 0; i < n; i++)
    {
        const nw_item_t *o = &items[others[i]];
        if (o->id == it->id)
        {
            continue;
        }
        if (!header)
        {
            ui_section(page, "La stessa storia su altri giornali");
            header = true;
        }
        char osource[28], owhen[24], ometa[64];
        feed_name(o->feed, osource, sizeof(osource));
        nw_time_text(o->when, owhen, sizeof(owhen));
        snprintf(ometa, sizeof(ometa), "%s · %s", osource, owhen);
        headline(page, false, topic_color(o->topic), o->title, ometa, open_item, (void *)(uintptr_t)o->id);
    }
    nw_unlock();
    free(others);
    if (ui->query[0])
    {
        char text[120];
        snprintf(text, sizeof(text), "Per capire meglio: \"%s\"", ui->query);
        ui_section(page, text);
        ui_button(page, "Contesto (Wikipedia)", 0x3D6BFF, ask_context, NULL);
        ui_button(page, "Cronologia", C_ACCENT, ask_timeline, NULL);
    }
    ui_text(page, "Il link all'articolo completo è nella dashboard.", true);
}

/* ------------------------------------------------------------- CONTEXT */

static void context(news_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Contesto");
    if (nw_context_running())
    {
        char text[120];
        snprintf(text, sizeof(text), "Cerco \"%s\" su Wikipedia...", ui->query);
        ui_text(page, text, true);
        return;
    }
    char *text = heap_caps_malloc(1200, MALLOC_CAP_SPIRAM);
    char title[80];
    if (text == NULL)
    {
        return;
    }
    nw_lock();
    nw_context_result(title, sizeof(title), text, 1200);
    nw_unlock();
    lv_obj_t *t = label(page, ui_font_title, C_INK, title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, card_width());
    lv_obj_t *body = label(page, UI_FONT_BODY, 0xD9D4CA, text);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(body, card_width());
    free(text);
    ui_text(page, "Da Wikipedia in italiano (CC BY-SA).", true);
    ui_button(page, "Cronologia nelle notizie", C_ACCENT, ask_timeline, NULL);
}

/* ------------------------------------------------------------ TIMELINE */

static void timeline(news_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Cronologia");
    char text[120];
    snprintf(text, sizeof(text), "\"%s\"", ui->query);
    lv_obj_t *q = label(page, UI_FONT_BODY, C_ACCENT, text);
    lv_obj_set_width(q, card_width());
    if (nw_search_running())
    {
        ui_text(page, "Cerco nell'archivio...", true);
        return;
    }
    nw_lock();
    int count;
    char query[80];
    const nw_hit_t *hits = nw_search_result(&count, query, sizeof(query));
    int64_t last_day = -1;
    for (int i = 0; i < count; i++)
    {
        const nw_hit_t *h = &hits[i];
        time_t t = (time_t)h->when;
        struct tm tm;
        localtime_r(&t, &tm);
        int64_t day = (int64_t)(tm.tm_year * 400 + tm.tm_yday);
        if (day != last_day)
        {
            static const char *const months[] = {"gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic"};
            snprintf(text, sizeof(text), "%d %s %d", tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900);
            ui_section(page, text);
            last_day = day;
        }
        char meta[48];
        snprintf(meta, sizeof(meta), "%02d:%02d · %s", tm.tm_hour, tm.tm_min, h->source);
        headline(page, false, topic_color(h->topic), h->title, meta, NULL, NULL);
    }
    nw_unlock();
    if (count == 0)
    {
        ui_text(page, "Nessun titolo in archivio negli ultimi mesi.", true);
    }
    ui_button(page, "Cerca in tutto l'anno", 0x3D6BFF, search_more, NULL);
    ui_button(page, "Contesto (Wikipedia)", 0x3D6BFF, ask_context, NULL);
}

/* -------------------------------------------------------------- TRENDS */

static void trends(news_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Classifiche");
    static const char *const names[NW_PERIODS] = {"Oggi", "Settimana", "Mese", "Anno"};
    lv_obj_t *row = pill_row(page);
    for (int p = 0; p < NW_PERIODS; p++)
    {
        pill(row, names[p], p == ui->period, set_period, (void *)(intptr_t)p);
    }
    nw_lock();
    int count;
    const nw_term_t *terms = nw_trends((nw_period_t)ui->period, &count);
    int top = count > 0 ? terms[0].count : 1;
    int width = card_width();
    for (int i = 0; i < count && i < 20; i++)
    {
        lv_obj_t *card = lv_button_create(page);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, width, 52);
        lv_obj_set_style_bg_color(card, ui_color(C_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(card, ui_color(C_LINE), LV_STATE_PRESSED);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_add_event_cb(card, open_term, LV_EVENT_CLICKED, NULL);
        lv_port_set_focusable(card);
        /* child 0 = the word (open_term reads it) */
        lv_obj_t *w = label(card, UI_FONT_BODY, C_INK, terms[i].word);
        lv_obj_align(w, LV_ALIGN_LEFT_MID, 14, -6);
        char n[12];
        snprintf(n, sizeof(n), "%u", terms[i].count);
        lv_obj_t *c = label(card, UI_FONT_SMALL, C_DIM, n);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, -14, -6);
        lv_obj_t *bar = lv_obj_create(card);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, (width - 28) * terms[i].count / (top ? top : 1) + 4, 4);
        lv_obj_set_style_bg_color(bar, ui_color(i < 3 ? C_ACCENT : 0x6B6760), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 14, -8);
    }
    nw_unlock();
    if (count == 0)
    {
        ui_text(page, ui->period == NW_DAY ? "Le parole del giorno arrivano con le notizie."
                                           : "Servono più giorni di notizie: la classifica cresce con l'archivio.",
                true);
    }
    else
    {
        ui_text(page, "Tocca una parola per la sua cronologia.", true);
    }
}

/* ---------------------------------------------------------------- MENU */

static void menu(news_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Notizie");
    nw_settings_t s;
    nw_settings(&s);
    nw_status_t st;
    nw_status(&st);
    ui_row(page, LV_SYMBOL_WIFI, C_ACCENT, "Dashboard", NULL, open_view, (void *)(intptr_t)DASH);
    ui_row(page, LV_SYMBOL_KEYBOARD, 0x3D6BFF, "Cerca nell'archivio", NULL, ask_search, NULL);
    ui_row(page, LV_SYMBOL_REFRESH, C_GOOD, "Aggiorna ora", NULL, refresh, NULL);
    char every[24];
    if (s.every_hours)
    {
        snprintf(every, sizeof(every), "ogni %d h", s.every_hours);
    }
    else
    {
        snprintf(every, sizeof(every), "solo app aperta");
    }
    ui_row(page, LV_SYMBOL_LOOP, 0x8E8E93, "Raccolta", every, cycle_background, NULL);
    char text[160];
    snprintf(text, sizeof(text), "%d notizie in memoria (3 giorni), %d storie. %s", st.items, st.stories, st.message);
    ui_text(page, text, true);
    ui_text(page, "La raccolta in background tiene l'archivio completo per le classifiche; "
                  "con il Bluetooth acceso aspetta l'apertura dell'app.", true);
}

static void dash(news_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Dashboard");
    appkit_dashboard_page(page, ui_color(C_ACCENT));
}

/* -------------------------------------------------------------- render */

static void render(news_ui_t *ui)
{
    if (ui->content != NULL)
    {
        lv_obj_delete(ui->content);
    }
    ui->status = NULL;
    ui->content = lv_obj_create(ui->screen);
    lv_obj_remove_style_all(ui->content);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->content, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->content, LV_OPA_COVER, 0);
    lv_obj_move_to_index(ui->content, 0);
    nw_status_t st;
    nw_status(&st);
    ui->revision = st.revision;
    ui->phase = st.phase;
    switch (ui->view)
    {
    case FRONT: front(ui); break;
    case SECTIONS: sections(ui); break;
    case SECTION: section(ui); break;
    case STORY: story(ui); break;
    case CONTEXT: context(ui); break;
    case TIMELINE: timeline(ui); break;
    case TRENDS: trends(ui); break;
    case MENU: menu(ui); break;
    default: dash(ui); break;
    }
}

static void tick(lv_timer_t *timer)
{
    news_ui_t *ui = lv_timer_get_user_data(timer);
    nw_status_t st;
    nw_status(&st);
    bool busy = st.phase == NW_WAITING_WIFI || st.phase == NW_FETCHING || st.phase == NW_ANALYSING;
    if (ui->border != NULL)
    {
        lv_obj_set_hidden(ui->border, !busy);
        if (busy)
        {
            ui_border_progress_set(ui->border, st.percent, 100);
        }
    }
    if (ui->status != NULL && st.revision != ui->revision)
    {
        lv_label_set_text(ui->status, st.message);
    }
    /* Whole page again only when a job ends (no jumps while reading). */
    bool ended = st.phase != ui->phase && (st.phase == NW_IDLE || st.phase == NW_FAILED);
    bool waiting = (ui->view == CONTEXT || ui->view == TIMELINE) && st.revision != ui->revision &&
                   !nw_search_running() && !nw_context_running();
    if (ended && (ui->view == FRONT || ui->view == SECTIONS || ui->view == SECTION || ui->view == TRENDS ||
                  ui->view == CONTEXT || ui->view == TIMELINE))
    {
        render(ui);
    }
    else if (waiting)
    {
        render(ui);
    }
    ui->phase = st.phase;
    ui->revision = st.revision;
}

bool news_ui_back(void)
{
    news_ui_t *ui = s_ui;
    if (ui == NULL || ui->depth == 0)
    {
        return false;   /* internal app: the app manager closes it */
    }
    ui->view = ui->history[--ui->depth];
    render(ui);
    return true;
}

void news_ui_create(lv_obj_t *screen)
{
    news_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    if (ui == NULL)
    {
        ui_text(ui_page(screen, "Notizie"), "Memoria insufficiente.", true);
        return;
    }
    s_ui = ui;
    ui->screen = screen;
    lv_obj_set_style_bg_color(screen, ui_color(C_BG), 0);
    render(ui);
    ui->border = ui_border_progress_create(screen, C_ACCENT);
    if (ui->border != NULL)
    {
        lv_obj_set_hidden(ui->border, true);
    }
    ui->timer = lv_timer_create(tick, 500, ui);
}

void news_ui_destroy(void)
{
    news_ui_t *ui = s_ui;
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
