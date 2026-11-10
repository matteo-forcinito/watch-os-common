/*
 * Deck: the PC page. With the agent (tools/deck_agent) running on the PC, the
 * watch knows the focused application and shows a touchbar for it, plus what
 * is playing, CPU/RAM and the microphone. Without the agent the page keeps a
 * generic touchbar: shortcuts are always HID key presses sent by the watch.
 *
 * Messages are small JSON objects on the Deck GATT channel (hid_link.h):
 *   PC -> watch   {"t":"ctx","app":"code","title":"main.c - VS Code"}
 *                 {"t":"media","title":"..","artist":"..","playing":1}
 *                 {"t":"sys","cpu":12,"ram":48,"mic":1}      mic: 1 live, 0 muted, -1 unknown
 *                 {"t":"res","ok":1,"msg":"PC bloccato"}
 *                 {"t":"cmd","c":"usb"|"ble"}           move Remote to the other transport
 *                 {"t":"ping"} every 2 s, {"t":"bye"} on exit: the watch shows the touchbar
 *                 only while the agent is heard
 *   watch -> PC   {"t":"hello","v":0,"name":".."}
 *                 {"t":"prof","n":"VS Code"}            touchbar chosen for the last ctx
 *                 {"t":"act","a":"lock"|"mic"|"display_off"|"taskmgr"}
 */
#include "remote.h"
#include "remote_bar_layout.h"
#include "core/lv_port.h"
#include "hardware/board.h"
#include "services/sound.h"
#include "ui/ui.h"
#include "ui/keyboard_layout.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "deck_core/deck_core.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "extapp_sdk.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



/* {"t":"cmd","c":"usb"|"ble"}: the agent's tray menu moves Remote to the other
 * transport. Saved now, restarted shortly after so the agent gets its answer. */
static void restart_now(void *arg)
{
    (void)arg;
    extapp_restart_self();
}

/* Runs in the esp_timer task, never in the USB or Bluetooth task: saving to NVS
 * there would stall the link that carries the answer. */
static _Atomic int s_command = HID_LINK_NONE;

static void command(void *arg)
{
    (void)arg;
    hid_link_transport_t to = (hid_link_transport_t)atomic_exchange(&s_command, HID_LINK_NONE);
    if (to == HID_LINK_NONE)
        return;
    if (to == HID_LINK_SATELLITE || (to == HID_LINK_USB && !board_info()->has_usb_otg))
    {
        hid_deck_send("{\"t\":\"res\",\"ok\":0,\"msg\":\"Trasporto non disponibile\"}");
        return;
    }
    remote_settings_t settings = *remote_settings();
    settings.transport = (uint8_t)to;
    remote_settings_save(&settings);
    hid_deck_send(to == HID_LINK_USB ? "{\"t\":\"res\",\"ok\":1,\"msg\":\"Passo a USB\"}"
                                     : "{\"t\":\"res\",\"ok\":1,\"msg\":\"Passo a Bluetooth\"}");
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = restart_now, .name = "deck_restart"};
    if (timer == NULL && esp_timer_create(&args, &timer) != ESP_OK)
        return;
    esp_timer_start_once(timer, 500 * 1000);
}

static void command_later(const char *json)
{
    static esp_timer_handle_t timer;
    char what[8] = "";
    deck_json_text(json, "c", what, sizeof(what));
    /* An unknown transport becomes HID_LINK_SATELLITE: answered as not available. */
    atomic_store(&s_command, strcmp(what, "usb") == 0   ? HID_LINK_USB
                             : strcmp(what, "ble") == 0 ? HID_LINK_BLE
                                                        : HID_LINK_SATELLITE);
    const esp_timer_create_args_t args = {.callback = command, .name = "deck_cmd"};
    if (timer == NULL && esp_timer_create(&args, &timer) != ESP_OK)
        return;
    esp_timer_stop(timer);
    esp_timer_start_once(timer, 1000);
}

void remote_deck_init(void)
{
    deck_core_init(command_later);
}

static bool s_hello_sent;

static void send_action(const char *action)
{
    if (!deck_send_action(action))
        ui_toast("Avvia l'agente sul PC");
}


/* ---- open windows ----
 *
 * "App aperte": the agent lists the PC's windows, a tap brings one forward.
 * The switch that needs no agent is "Cambia app" (Alt held across taps). */

typedef struct
{
    lv_obj_t *status, *list;
    lv_timer_t *timer;
    uint32_t version;
    deck_item_t windows[DECK_LIST_MAX];
} windows_ui_t;

static void window_clicked(lv_event_t *e)
{
    char action[24];
    snprintf(action, sizeof(action), "focus:%d", (int)(intptr_t)lv_event_get_user_data(e));
    send_action(action);
    app_back();
}

static void windows_tick(lv_timer_t *timer)
{
    windows_ui_t *ui = lv_timer_get_user_data(timer);
    uint32_t version;
    int count = deck_list(DECK_LIST_WINDOWS, ui->windows, DECK_LIST_MAX, &version);
    if (version == ui->version)
        return;
    ui->version = version;
    lv_obj_clean(ui->list);
    lv_label_set_text(ui->status, count ? "Tocca la finestra da portare davanti." : "Nessuna finestra aperta.");
    for (int i = 0; i < count; i++)
    {
        const deck_item_t *w = &ui->windows[i];
        const deck_profile_t *p = deck_profile_for(w->app, w->title);
        ui_row(ui->list, p->icon, p->color, w->title[0] ? w->title : w->app, p->name[0] ? p->name : w->app,
               window_clicked, (void *)(intptr_t)i);
    }
}

static void windows_deleted(lv_event_t *e)
{
    windows_ui_t *ui = lv_event_get_user_data(e);
    lv_timer_delete(ui->timer);
    free(ui);
}

static void windows_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    windows_ui_t *ui = calloc(1, sizeof(*ui));
    if (ui == NULL)
        return;
    lv_obj_t *page = remote_page(screen, "App aperte");
    ui->status = ui_text(page, "Chiedo l'elenco al PC...", true);
    ui->list = lv_obj_create(page);
    lv_obj_remove_style_all(ui->list);
    lv_obj_set_size(ui->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ui->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui->list, 8, 0);
    deck_list(DECK_LIST_WINDOWS, ui->windows, 0, &ui->version); /* only a list newer than now */
    if (!deck_send_action("windows"))
        lv_label_set_text(ui->status, "Avvia Watch Deck sul PC per vedere le finestre.");
    ui->timer = lv_timer_create(windows_tick, 150, ui);
    lv_obj_add_event_cb(screen, windows_deleted, LV_EVENT_DELETE, ui);
}

static const app_t s_windows_app = {.id = "remote.deck.windows",
                                    .name = "App aperte",
                                    .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON,
                                    .create = windows_create};

/* ---- page ----
 *
 *   header      back, profile icon and name, page n/m (tap for the next page)
 *               window title
 *   grid        keys of the focused application; swipe or knob for more pages.
 *               The last pages are always Sistema (lock, display, desktop, snap...)
 *   footer      what is playing (or CPU/RAM); previous, play, next, microphone
 *
 * Geometry follows the screen as LVGL draws it (rotation included):
 * landscape 3x3 keys, portrait 2x4, round panels 2x4 on circle chords.
 * Without the agent the page is a connection guide (cable or pairing, then agent).
 * The Deck channel works on both transports: GATT on Bluetooth, vendor HID report on USB. */

#define GRID_MAX 9

typedef enum
{
    VIEW_NONE,
    VIEW_CABLE, /* USB: cable not connected to a PC */
    VIEW_PAIR,  /* Bluetooth: no host connected */
    VIEW_AGENT,
    VIEW_LIVE,
} deck_view_t;

typedef struct
{
    lv_obj_t *guide, *guide_icon, *guide_title, *guide_text, *guide_button;
    lv_obj_t *live, *icon, *app, *pager, *title, *keys[GRID_MAX], *media, *mic;
    lv_timer_t *timer;
    const deck_profile_t *profile;
    char app_name[24];
    int page, per_page;
    deck_view_t view;
    uint32_t seen;
    /* Agent lists, copied when they change (PSRAM): own shortcuts of the app in front, quick launch. */
    deck_item_t *mine, *launch;
    int mine_count, launch_count;
    uint32_t mine_seen, launch_seen;
    /* Git (VS Code in front) and CI lists, parsed when they change. */
    deck_item_t *git, *ci;
    int git_count, ci_count;
    uint32_t git_seen, ci_seen;
    char git_repo[48], git_branch[48], git_msg[48];
    int git_changed, git_ahead, git_behind, git_conflicts, git_stash, git_untracked;
    int branch_first; /* index of the first "br" row in `git` */
    char ci_label[DECK_LIST_MAX][64];
    char window_title[64];
    uint8_t alerts_seen;
    bool alerts_known;
} deck_page_t;

/* Sections, in page order. Swipes run through all of them; a tap on the header opens the index. */
typedef enum
{
    SEC_MINE,   /* the user's shortcuts for this app (agent UI) */
    SEC_APP,    /* built-in profile of the app in front */
    SEC_GIT,    /* repository of the VS Code window in front */
    SEC_CI,     /* latest CI run of the chosen repositories */
    SEC_LAUNCH, /* quick launch: apps and sites */
    SEC_SYSTEM, /* switch app, windows, desktops, lock... */
    SEC_COUNT,
} deck_section_t;

static deck_page_t *s_page;

/* Git section keys, in tile order: the actions are "git:<action>". */
typedef enum
{
    GIT_PULL,
    GIT_PUSH,
    GIT_FETCH,
    GIT_COMMIT,
    GIT_STASH,
    GIT_POP,
    GIT_BRANCH,
    GIT_DIFF,
    GIT_KEYS,
} git_key_t;
static const char *const s_git_labels[GIT_KEYS] = {"Pull", "Push", "Fetch", "Commit", "Metti da parte",
                                                   "Riprendi", "Branch", "Modifiche"};
static const char *const s_git_actions[GIT_KEYS] = {"pull", "push", "fetch", "commit", "stash", "pop", NULL, "diff"};

static uint32_t section_color(const deck_page_t *page, deck_section_t section)
{
    static const uint32_t colors[] = {UI_COLOR_PINK, 0, UI_COLOR_ORANGE, UI_COLOR_GREEN, UI_COLOR_TEAL, UI_COLOR_GRAY};
    return section == SEC_APP ? page->profile->color : colors[section];
}

static const char *section_icon(const deck_page_t *page, deck_section_t section)
{
    static const char *const icons[] = {LV_SYMBOL_EDIT, NULL, LV_SYMBOL_SHUFFLE, LV_SYMBOL_REFRESH, LV_SYMBOL_PLAY,
                                        LV_SYMBOL_SETTINGS};
    return section == SEC_APP ? page->profile->icon : icons[section];
}

/* CI state of an item: tile border and word. */
static uint32_t ci_color(const deck_item_t *item)
{
    return strcmp(item->app, "run") == 0  ? UI_COLOR_ORANGE
           : strcmp(item->app, "ok") == 0 ? UI_COLOR_GREEN
           : strcmp(item->app, "ko") == 0 ? UI_COLOR_RED
                                          : UI_COLOR_GRAY;
}

static const char *ci_word(const deck_item_t *item)
{
    return strcmp(item->app, "run") == 0    ? "In corso"
           : strcmp(item->app, "ok") == 0   ? "OK"
           : strcmp(item->app, "ko") == 0   ? "Fallita"
           : strcmp(item->app, "stop") == 0 ? "Annullata"
                                            : "Nessuna run";
}


static int system_count(void)
{
    int count;
    deck_system_keys(&count);
    return count;
}

static int pages_of(int count, int per)
{
    return (count + per - 1) / per;
}

static int section_items(const deck_page_t *page, deck_section_t section)
{
    switch (section)
    {
    case SEC_MINE:
        return page->mine_count;
    case SEC_APP:
        return page->profile->count;
    case SEC_GIT:
        return page->git_count > 0 ? GIT_KEYS : 0;
    case SEC_CI:
        return page->ci_count;
    case SEC_LAUNCH:
        return page->launch_count;
    default:
        return system_count();
    }
}

static int section_pages(const deck_page_t *page, deck_section_t section)
{
    return pages_of(section_items(page, section), page->per_page);
}

static int page_count(const deck_page_t *page)
{
    int pages = 0;
    for (int i = 0; i < SEC_COUNT; i++)
        pages += section_pages(page, (deck_section_t)i);
    return pages > 0 ? pages : 1;
}

static int section_first_page(const deck_page_t *page, deck_section_t section)
{
    int first = 0;
    for (int i = 0; i < (int)section; i++)
        first += section_pages(page, (deck_section_t)i);
    return first;
}

/* Section of the current page, and the page number inside it. */
static deck_section_t current_section(const deck_page_t *page, int *local)
{
    int p = page->page;
    for (int i = 0; i < SEC_COUNT; i++)
    {
        int n = section_pages(page, (deck_section_t)i);
        if (p < n)
        {
            if (local != NULL)
                *local = p;
            return (deck_section_t)i;
        }
        p -= n;
    }
    if (local != NULL)
        *local = 0;
    return SEC_SYSTEM;
}

/* Item index (inside its section) in `slot` of the current page, or -1 for an empty slot. */
static int item_at(const deck_page_t *page, int slot, deck_section_t *section)
{
    int local;
    *section = current_section(page, &local);
    int i = local * page->per_page + slot;
    return i < section_items(page, *section) ? i : -1;
}

static const deck_key_t *key_of(const deck_page_t *page, deck_section_t section, int i)
{
    if (section == SEC_APP)
        return &page->profile->keys[i];
    int count;
    return section == SEC_SYSTEM ? &deck_system_keys(&count)[i] : NULL;
}

static const char *label_of(const deck_page_t *page, deck_section_t section, int i)
{
    if (section == SEC_MINE)
        return page->mine[i].title;
    if (section == SEC_LAUNCH)
        return page->launch[i].title;
    if (section == SEC_GIT)
        return s_git_labels[i];
    if (section == SEC_CI)
        return page->ci_label[i];
    return key_of(page, section, i)->label;
}

static const char *section_name(const deck_page_t *page, deck_section_t section)
{
    static const char *const names[] = {"Le mie", NULL, "Git", "CI", "Avvio", "Sistema"};
    if (section == SEC_APP)
        return page->profile->name[0] ? page->profile->name : page->app_name;
    if (section == SEC_GIT && page->git_repo[0])
        return page->git_repo;
    return names[section];
}

static int32_t screen_w(void)
{
    return lv_display_get_horizontal_resolution(lv_display_get_default());
}

static int32_t screen_h(void)
{
    return lv_display_get_vertical_resolution(lv_display_get_default());
}

/* A horizontal strip [top, top + height) of the screen, inset to the circle on round panels. */
static ui_key_rect_t strip(int32_t top, int32_t height)
{
    return ui_keyboard_band(screen_w(), screen_h(), board_info()->round, top, height, 8);
}

/* A single-line label that never grows: fixed height, ellipsis. */
static lv_obj_t *line(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, const lv_font_t *font, uint32_t color,
                      lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, lv_font_get_line_height(font));
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, ui_color(color), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, "");
    return label;
}

static lv_obj_t *tile(lv_obj_t *parent, ui_key_rect_t r, const char *text, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, r.x + 3, r.y + 3);
    lv_obj_set_size(b, r.w - 6, r.h - 6);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, ui_color(UI_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, ui_accent(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, ui_color(UI_COLOR_SEPARATOR), 0);
    lv_obj_set_scrollable(b, false);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_port_set_focusable(b);
    /* Up to two centred lines: names are never cut on the panels Deck targets. */
    lv_obj_t *label = lv_label_create(b);
    lv_obj_set_width(label, r.w - 16);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, r.h < 50 || r.w < 110 ? UI_FONT_SMALL : UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(label, ui_color(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_line_space(label, -2, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    return b;
}

static lv_obj_t *layer(lv_obj_t *screen)
{
    lv_obj_t *obj = lv_obj_create(screen);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(obj, false);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return obj;
}

/* Body font unless a word would be broken across lines (labels are ASCII). */
static void fit_label(lv_obj_t *label, const char *text)
{
    int32_t room = lv_obj_get_width(label);
    int32_t word = 0;
    const lv_font_t *font = UI_FONT_BODY;
    for (const char *c = text;; c++)
    {
        if (*c == ' ' || *c == '\0')
        {
            if (word > room)
            {
                font = UI_FONT_SMALL;
                break;
            }
            if (*c == '\0')
                break;
            word = 0;
            continue;
        }
        word += lv_font_get_glyph_width(UI_FONT_BODY, (uint32_t)(uint8_t)*c, (uint32_t)(uint8_t)c[1]);
    }
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_text(label, text);
}

static void show_page(deck_page_t *page)
{
    const deck_profile_t *p = page->profile;
    page->page = LV_MIN(page->page, page_count(page) - 1);
    deck_section_t section = current_section(page, NULL);
    uint32_t color = section == SEC_SYSTEM ? UI_COLOR_SEPARATOR : section_color(page, section);

    for (int i = 0; i < page->per_page; i++)
    {
        deck_section_t at;
        int item = item_at(page, i, &at);
        if (item < 0)
        {
            lv_obj_add_flag(page->keys[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(page->keys[i], LV_OBJ_FLAG_HIDDEN);
        fit_label(lv_obj_get_child(page->keys[i], 0), label_of(page, at, item));
        uint32_t border = color;
        if (at == SEC_CI)
            border = ci_color(&page->ci[item]);
        else if (at == SEC_GIT && ((item == GIT_PUSH && page->git_ahead) || (item == GIT_PULL && page->git_behind) ||
                                   (item == GIT_COMMIT && page->git_changed) || (item == GIT_POP && page->git_stash)))
            border = UI_COLOR_TEXT; /* what is worth doing now stands out */
        lv_obj_set_style_border_color(page->keys[i], ui_color(border), 0);
        lv_obj_set_style_border_width(page->keys[i], section == SEC_SYSTEM ? 1 : 2, 0);
    }

    lv_label_set_text(page->app, section_name(page, section));
    lv_label_set_text(lv_obj_get_child(page->icon, 0), section_icon(page, section));
    ui_icon_bubble_set_color(page->icon, section_color(page, section));
    lv_label_set_text_fmt(page->pager, "%d/%d", page->page + 1, page_count(page));
    (void)p;

    /* The line under the header: window title, or the state of the section shown. */
    char line[160];
    if (section == SEC_GIT)
    {
        /* Branch (47 bytes at most) plus four short parts: fits the 160 bytes. */
        char parts[4][24] = {"", "", "", ""};
        if (page->git_conflicts)
            snprintf(parts[0], sizeof(parts[0]), " · %d conflitti", page->git_conflicts);
        if (page->git_changed)
            snprintf(parts[1], sizeof(parts[1]), " · %d modifiche", page->git_changed);
        else
            snprintf(parts[1], sizeof(parts[1]), " · pulito");
        if (page->git_ahead)
            snprintf(parts[2], sizeof(parts[2]), " · %d da inviare", page->git_ahead);
        if (page->git_behind)
            snprintf(parts[3], sizeof(parts[3]), " · %d da ricevere", page->git_behind);
        snprintf(line, sizeof(line), "%.47s%s%s%s%s", page->git_branch, parts[0], parts[1], parts[2], parts[3]);
    }
    else if (section == SEC_CI)
    {
        int ok = 0, running = 0, failed = 0;
        for (int i = 0; i < page->ci_count; i++)
        {
            ok += strcmp(page->ci[i].app, "ok") == 0;
            running += strcmp(page->ci[i].app, "run") == 0;
            failed += strcmp(page->ci[i].app, "ko") == 0;
        }
        snprintf(line, sizeof(line), "%d ok · %d in corso · %d fallite", ok, running, failed);
    }
    else
        snprintf(line, sizeof(line), "%s", page->window_title);
    if (strcmp(lv_label_get_text(page->title), line) != 0)
        lv_label_set_text(page->title, line);
}

static void set_profile(deck_page_t *page, const deck_profile_t *profile, const char *app)
{
    snprintf(page->app_name, sizeof(page->app_name), "%s", app[0] ? app : "PC");
    if (page->profile != profile)
    {
        page->profile = profile;
        page->page = 0; /* a new application starts on its own first page */
    }
    show_page(page);
}

static void turn_page(deck_page_t *page, int delta)
{
    int pages = page_count(page);
    page->page = ((page->page + delta) % pages + pages) % pages;
    show_page(page);
}

/* Horizontal drags turn the page wherever they start, keys included: LVGL's
 * gesture event does not reach the page from a pressed key. */
static lv_point_t s_press_at;
static bool s_dragged;

static void drag_event(lv_event_t *e)
{
    lv_indev_t *input = lv_indev_active();
    if (input == NULL || s_page == NULL || s_page->profile == NULL)
        return;
    lv_point_t at;
    lv_indev_get_point(input, &at);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED)
    {
        s_press_at = at;
        s_dragged = false;
        return;
    }
    int32_t dx = at.x - s_press_at.x, dy = at.y - s_press_at.y;
    if (LV_ABS(dx) > 50 && LV_ABS(dx) > 2 * LV_ABS(dy))
    {
        s_dragged = true; /* the key under the finger must not fire */
        turn_page(s_page, dx < 0 ? 1 : -1);
    }
}

static void track_drags(lv_obj_t *obj)
{
    lv_obj_add_event_cb(obj, drag_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, drag_event, LV_EVENT_RELEASED, NULL);
}

/* ---- Git and CI ----
 *
 * Commit and Push ask first; the branch list is a page of its own. The agent
 * runs every command with a time limit and answers with a toast. */

static void git_confirmed(bool ok, void *arg)
{
    if (ok)
        send_action(arg);
}

static void ci_confirmed(bool ok, void *arg)
{
    char action[16];
    snprintf(action, sizeof(action), "ci:%d", (int)(intptr_t)arg);
    if (ok)
        send_action(action);
}

typedef struct
{
    deck_item_t rows[DECK_LIST_MAX];
    int first, count;
} branches_ui_t;

static void branch_clicked(lv_event_t *e)
{
    char action[24];
    snprintf(action, sizeof(action), "git:br:%d", (int)(intptr_t)lv_event_get_user_data(e));
    send_action(action);
    app_back();
}

static void branches_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_t *page = remote_page(screen, "Branch");
    if (s_page == NULL || s_page->git_count == 0)
    {
        ui_text(page, "Nessun repository in VS Code.", true);
        return;
    }
    ui_text(page, "Tocca il branch su cui passare.", true);
    for (int i = s_page->branch_first, n = 0; i < s_page->git_count; i++, n++)
    {
        const char *name = s_page->git[i].title;
        bool current = strcmp(name, s_page->git_branch) == 0;
        ui_row(page, current ? LV_SYMBOL_OK : LV_SYMBOL_SHUFFLE, current ? UI_COLOR_GREEN : UI_COLOR_ORANGE, name,
               current ? "attuale" : NULL, current ? NULL : branch_clicked, (void *)(intptr_t)n);
    }
}

static const app_t s_branches_app = {.id = "remote.deck.branches",
                                     .name = "Branch",
                                     .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON,
                                     .create = branches_create};

/* ---- Agent management ----
 *
 * Shows whether Watch Deck runs on the PC and its version, and over the Deck
 * channel can restart or stop it. On boards with a native USB port it can also
 * install the agent from the SD card (deck_install.c), so nothing is typed on
 * the PC. Reachable from the sections index and from the connection guide. */

static void restart_confirmed(bool ok, void *arg)
{
    (void)arg;
    if (ok)
        send_action("restart");
}

static void quit_confirmed(bool ok, void *arg)
{
    (void)arg;
    if (ok)
        send_action("quit");
}

static void manage_restart(lv_event_t *e)
{
    (void)e;
    ui_confirm("Riavvia agente", "Riavviare Watch Deck sul PC?", "Riavvia", UI_COLOR_ORANGE, restart_confirmed, NULL);
}

static void manage_quit(lv_event_t *e)
{
    (void)e;
    ui_confirm("Esci agente", "Chiudere Watch Deck sul PC? Il Deck resta senza agente finché non lo riavvii dal PC.",
               "Esci", UI_COLOR_RED, quit_confirmed, NULL);
}

static void drive_confirmed(bool ok, void *arg)
{
    (void)arg;
    if (ok)
        remote_drive_request(); /* restarts Remote as a USB drive; does not return */
}

static void manage_install(lv_event_t *e)
{
    (void)e;
    ui_confirm("Apri la SD sul PC",
               "Remote si riavvia come chiavetta USB: sul PC apri la cartella deck e avvia l'installer. "
               "Tastiera e mouse tornano quando tocchi Fine.",
               "Apri", UI_COLOR_BLUE, drive_confirmed, NULL);
}

static void manage_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_t *page = remote_page(screen, "Agente PC");
    bool alive = deck_agent_alive();
    char version[16] = "";
    deck_agent_version(version, sizeof(version));
    char line[96];
    if (alive)
        snprintf(line, sizeof(line), "Watch Deck attivo%s%s.", version[0] ? " · v" : "", version);
    else
        snprintf(line, sizeof(line), "Watch Deck non in esecuzione sul PC.");
    ui_section(page, "Stato");
    ui_text(page, line, false);
    if (alive)
    {
        ui_button(page, "Riavvia agente", UI_COLOR_ORANGE, manage_restart, NULL);
        ui_button(page, "Esci agente", UI_COLOR_RED, manage_quit, NULL);
    }
    ui_section(page, "Installazione");
    int found = remote_drive_installers();
    char list[96];
    if (found)
        snprintf(list, sizeof(list), "Installer sulla microSD:%s%s%s.", found & 1 ? " Windows" : "",
                 found & 2 ? " macOS" : "", found & 4 ? " Linux" : "");
    else
        snprintf(list, sizeof(list), "Nessun installer sulla microSD (cartella deck).");
    ui_text(page, list, false);
    if (remote_drive_supported())
    {
        ui_button(page, alive ? "Reinstalla sul PC" : "Installa sul PC", UI_COLOR_BLUE, manage_install, NULL);
        ui_text(page, "Collega il watch al PC con il cavo USB: la microSD compare come chiavetta, tu avvii "
                      "l'installer. Il watch non avvia niente da solo.", true);
    }
    else
        ui_text(page, "Questo dispositivo non ha una porta USB per i dati: prendi l'installer dalla cartella "
                      "deck della microSD (lettore SD) o dall'app Web di Watch OS (File).", true);
}

static const app_t s_manage_app = {.id = "remote.deck.agent",
                                   .name = "Agente PC",
                                   .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON,
                                   .create = manage_create};

static void git_clicked(deck_page_t *page, int key)
{
    static char text[128];
    if (key == GIT_BRANCH)
    {
        app_open_app(&s_branches_app, NULL);
        return;
    }
    if (key == GIT_COMMIT)
    {
        if (page->git_changed == 0)
        {
            ui_toast("Niente da salvare");
            return;
        }
        snprintf(text, sizeof(text), "%d file in %s.\n«%s»", page->git_changed, page->git_branch,
                 page->git_msg[0] ? page->git_msg : "Update");
        ui_confirm("Commit", text, "Commit", UI_COLOR_ORANGE, git_confirmed, (void *)"git:commit");
        return;
    }
    if (key == GIT_PUSH)
    {
        if (page->git_ahead == 0)
            snprintf(text, sizeof(text), "Inviare %s?", page->git_branch);
        else
            snprintf(text, sizeof(text), "Inviare %d commit di %s?", page->git_ahead, page->git_branch);
        ui_confirm("Push", text, "Push", UI_COLOR_ORANGE, git_confirmed, (void *)"git:push");
        return;
    }
    char action[24];
    snprintf(action, sizeof(action), "git:%s", s_git_actions[key]);
    send_action(action);
}

static void key_clicked(lv_event_t *e)
{
    if (s_dragged)
    {
        s_dragged = false;
        return;
    }
    if (s_page == NULL || s_page->profile == NULL)
        return;
    deck_section_t section;
    int i = item_at(s_page, (int)(intptr_t)lv_event_get_user_data(e), &section);
    if (i < 0)
        return;
    char action[24];
    if (section == SEC_GIT)
    {
        git_clicked(s_page, i);
        return;
    }
    if (section == SEC_CI)
    {
        static char text[96];
        snprintf(text, sizeof(text), "%s", s_page->ci[i].title);
        ui_confirm("Riavvia run", text, "Riavvia", UI_COLOR_GREEN, ci_confirmed, (void *)(intptr_t)i);
        return;
    }
    if (section == SEC_MINE || section == SEC_LAUNCH)
    {
        /* The agent presses the keys or starts the app: it knows the PC better than HID codes. */
        snprintf(action, sizeof(action), "%s:%d", section == SEC_MINE ? "ck" : "launch", i);
        send_action(action);
        return;
    }
    const deck_key_t *key = key_of(s_page, section, i);
    if (key->action != NULL && strcmp(key->action, "@windows") == 0)
    {
        app_open_app(&s_windows_app, NULL);
        return;
    }
    if (key->action != NULL && key->action[0] != '@')
    {
        send_action(key->action);
        return;
    }
    remote_send_ok(deck_run_key(key, remote_settings()->host_os == 1));
}

static void action_clicked(lv_event_t *e)
{
    send_action(lv_event_get_user_data(e));
}

static void media_clicked(lv_event_t *e)
{
    remote_send_ok(hid_link_media((uint16_t)(intptr_t)lv_event_get_user_data(e)));
}

static void page_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *input = lv_indev_active();
    if (s_page == NULL || s_page->view != VIEW_LIVE || s_page->profile == NULL || input == NULL)
        return;
    lv_dir_t dir = lv_indev_get_gesture_dir(input);
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT)
        return;
    lv_indev_wait_release(input);
    turn_page(s_page, dir == LV_DIR_LEFT ? 1 : -1);
}

static void page_knob(lv_event_t *e)
{
    if (lv_port_focus_mode() || s_page == NULL || s_page->view != VIEW_LIVE || s_page->profile == NULL)
        return;
    lv_port_knob_t *knob = lv_event_get_param(e);
    knob->handled = true;
    turn_page(s_page, knob->delta > 0 ? 1 : -1);
}

/* ---- section index ---- */

static void section_chosen(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    app_back();
    if (choice == SEC_COUNT)
    {
        app_open_app(&s_windows_app, NULL);
        return;
    }
    if (choice == SEC_COUNT + 1)
    {
        app_open_app(&s_manage_app, NULL);
        return;
    }
    if (s_page != NULL && s_page->profile != NULL)
    {
        s_page->page = section_first_page(s_page, (deck_section_t)choice);
        show_page(s_page);
    }
}

static void sections_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_t *page = remote_page(screen, "Sezioni");
    if (s_page == NULL || s_page->profile == NULL)
        return;
    deck_section_t now = current_section(s_page, NULL);
    for (int i = 0; i < SEC_COUNT; i++)
    {
        int items = section_items(s_page, (deck_section_t)i);
        if (items == 0)
            continue;
        char value[32];
        snprintf(value, sizeof(value), "%s%d", i == (int)now ? LV_SYMBOL_OK " " : "", items);
        ui_row(page, section_icon(s_page, (deck_section_t)i), section_color(s_page, (deck_section_t)i),
               section_name(s_page, (deck_section_t)i), value, section_chosen, (void *)(intptr_t)i);
    }
    ui_row(page, LV_SYMBOL_LIST, UI_COLOR_BLUE, "App aperte", NULL, section_chosen, (void *)(intptr_t)SEC_COUNT);
    ui_row(page, LV_SYMBOL_SETTINGS, UI_COLOR_GRAY, "Agente PC", deck_agent_alive() ? LV_SYMBOL_OK : NULL,
           section_chosen, (void *)(intptr_t)(SEC_COUNT + 1));
    ui_text(page,
            "Le mie, Avvio e i repository CI si scelgono nell'app Watch Deck sul PC (tasto destro sull'icona > "
            "Apri Watch Deck). Git compare con VS Code davanti.",
            true);
}

static const app_t s_sections_app = {.id = "remote.deck.sections",
                                     .name = "Sezioni",
                                     .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON,
                                     .create = sections_create};

static void header_clicked(lv_event_t *e)
{
    (void)e;
    if (s_page != NULL && s_page->view == VIEW_LIVE && s_page->profile != NULL)
        app_open_app(&s_sections_app, NULL);
}

static void pager_clicked(lv_event_t *e)
{
    (void)e;
    if (s_page != NULL && s_page->profile != NULL)
        turn_page(s_page, 1);
}

static void show_guide(deck_page_t *page, deck_view_t view)
{
    static const struct
    {
        const char *icon, *title, *text;
        uint32_t color;
    } guides[] = {
        [VIEW_CABLE] = {LV_SYMBOL_USB, "Collega il cavo",
                        "Collega il watch al PC con il cavo USB. Funziona anche senza associazione Bluetooth.",
                        UI_COLOR_ORANGE},
        [VIEW_PAIR] = {LV_SYMBOL_BLUETOOTH, "Associa il PC",
                       "Bluetooth del PC > Aggiungi dispositivo. Se era già associato, rimuovilo e riassocialo.",
                       UI_COLOR_BLUE},
        [VIEW_AGENT] = {LV_SYMBOL_REFRESH, "Avvia Watch Deck",
                        "Avvia Watch Deck sul PC. Non è installato? Tocca il pulsante qui sotto.",
                        UI_COLOR_GREEN},
    };
    ui_icon_bubble_set_color(page->guide_icon, guides[view].color);
    lv_label_set_text(lv_obj_get_child(page->guide_icon, 0), guides[view].icon);
    lv_label_set_text(page->guide_title, guides[view].title);
    lv_label_set_text(page->guide_text, guides[view].text);
    /* The other transport is one tap away (USB only on boards with a native port). */
    bool usb = remote_settings()->transport == HID_LINK_USB;
    if (view == VIEW_AGENT)
    {
        lv_label_set_text(lv_obj_get_child(page->guide_button, 0), "Installa Watch Deck");
        lv_obj_remove_flag(page->guide_button, LV_OBJ_FLAG_HIDDEN);
    }
    else if (usb || board_info()->has_usb_otg)
    {
        lv_label_set_text(lv_obj_get_child(page->guide_button, 0), usb ? "Usa il Bluetooth" : "Usa il cavo USB");
        lv_obj_remove_flag(page->guide_button, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(page->guide_button, LV_OBJ_FLAG_HIDDEN);
    }
}

static void tick(lv_timer_t *timer)
{
    deck_page_t *page = lv_timer_get_user_data(timer);
    bool agent = deck_agent_alive();

    if (agent && !s_hello_sent)
    {
        char hello[64];
        snprintf(hello, sizeof(hello), "{\"t\":\"hello\",\"v\":1,\"name\":\"%s\"}", remote_device_name());
        s_hello_sent = hid_deck_send(hello);
    }
    if (!agent)
        s_hello_sent = false;

    deck_view_t view = hid_link_state() != HID_LINK_CONNECTED
                           ? (remote_settings()->transport == HID_LINK_USB ? VIEW_CABLE : VIEW_PAIR)
                       : !agent ? VIEW_AGENT
                                : VIEW_LIVE;
    bool bar = remote_bar_size(screen_w(), screen_h()) || remote_narrow_size(screen_w(), screen_h());
    if (bar)
        view = VIEW_LIVE; /* Generic HID shortcuts remain usable without the PC agent. */
    if (view != page->view)
    {
        page->view = view;
        page->seen = UINT32_MAX;
        if (view == VIEW_LIVE)
        {
            lv_obj_add_flag(page->guide, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(page->live, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            show_guide(page, view);
            lv_obj_add_flag(page->live, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(page->guide, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (view != VIEW_LIVE)
        return;

    deck_state_t st;
    deck_state_take(&st);

    /* Agent lists: quick launch any time, own shortcuts only for the app in front. */
    uint32_t version;
    bool lists = false;
    deck_list(DECK_LIST_APPS, page->launch, 0, &version);
    if (version != page->launch_seen)
    {
        page->launch_seen = version;
        page->launch_count = deck_list(DECK_LIST_APPS, page->launch, DECK_LIST_MAX, &version);
        lists = true;
    }
    deck_list(DECK_LIST_KEYS, page->mine, 0, &version);
    if (version != page->mine_seen || (st.version != page->seen && page->mine_count > 0))
    {
        page->mine_seen = version;
        page->mine_count = deck_list(DECK_LIST_KEYS, page->mine, DECK_LIST_MAX, &version);
        if (page->mine_count > 0 && strncmp(page->mine[0].app, st.app[0] ? st.app : page->app_name,
                                            sizeof(page->mine[0].app) - 1) != 0)
            page->mine_count = 0; /* shortcuts of another app: the new list is on its way */
        lists = true;
    }
    deck_list(DECK_LIST_GIT, page->git, 0, &version);
    if (version != page->git_seen)
    {
        page->git_seen = version;
        page->git_count = deck_list(DECK_LIST_GIT, page->git, DECK_LIST_MAX, &version);
        page->git_repo[0] = page->git_branch[0] = page->git_msg[0] = '\0';
        page->git_changed = page->git_ahead = page->git_behind = 0;
        page->git_conflicts = page->git_stash = page->git_untracked = 0;
        page->branch_first = page->git_count;
        for (int i = 0; i < page->git_count; i++)
        {
            const deck_item_t *row = &page->git[i];
            if (strcmp(row->app, "repo") == 0)
                snprintf(page->git_repo, sizeof(page->git_repo), "%s", row->title);
            else if (strcmp(row->app, "branch") == 0)
                snprintf(page->git_branch, sizeof(page->git_branch), "%s", row->title);
            else if (strcmp(row->app, "msg") == 0)
                snprintf(page->git_msg, sizeof(page->git_msg), "%s", row->title);
            else if (strcmp(row->app, "state") == 0)
                sscanf(row->title, "%d,%d,%d,%d,%d,%d", &page->git_changed, &page->git_ahead, &page->git_behind,
                       &page->git_conflicts, &page->git_stash, &page->git_untracked);
            else if (strcmp(row->app, "br") == 0 && page->branch_first == page->git_count)
                page->branch_first = i;
        }
        lists = true;
    }
    deck_list(DECK_LIST_CI, page->ci, 0, &version);
    if (version != page->ci_seen)
    {
        page->ci_seen = version;
        page->ci_count = deck_list(DECK_LIST_CI, page->ci, DECK_LIST_MAX, &version);
        for (int i = 0; i < page->ci_count; i++)
        {
            /* "repo · branch · 3m12s" -> "repo" + new line + "OK 3m12s" on the tile */
            char repo[48];
            snprintf(repo, sizeof(repo), "%s", page->ci[i].title);
            char *cut = strstr(repo, " · ");
            const char *time = strrchr(page->ci[i].title, ' ');
            if (cut != NULL)
                *cut = '\0';
            bool timed = cut != NULL && time != NULL && strcmp(page->ci[i].app, "off") != 0;
            snprintf(page->ci_label[i], sizeof(page->ci_label[i]), "%s\n%s%s", repo, ci_word(&page->ci[i]),
                     timed ? time : "");
        }
        lists = true;
    }
    if (lists && page->profile != NULL && st.version == page->seen)
        show_page(page);
    /* A CI run finished: the agent asks for a sound along with its message. */
    if (!page->alerts_known || st.alerts != page->alerts_seen)
    {
        if (page->alerts_known)
            sound_notification();
        page->alerts_seen = st.alerts;
        page->alerts_known = true;
    }

    if (st.message[0])
        ui_toast(st.message);
    if (bar && !agent)
    {
        snprintf(page->window_title, sizeof(page->window_title), "%s", hid_link_state() == HID_LINK_CONNECTED
                     ? "Scorciatoie HID · Watch Deck abilita il contesto PC" : remote_status_text());
        lv_label_set_text(page->title, page->window_title);
    }
    if (st.version == page->seen)
        return;
    page->seen = st.version;

    if (!bar || agent)
        snprintf(page->window_title, sizeof(page->window_title), "%s", st.title);
    set_profile(page, deck_profile_for(st.app, st.title), st.app);

    if (st.track[0])
        lv_label_set_text_fmt(page->media, "%s  %s%s%s", st.playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, st.track,
                              st.artist[0] ? " - " : "", st.artist);
    else if (st.cpu >= 0)
        lv_label_set_text_fmt(page->media, "CPU %d%%   RAM %d%%", st.cpu, st.ram);
    else
        lv_label_set_text(page->media, "");

    /* Microphone of the PC as the agent reads it: tap toggles it. */
    uint32_t mic_color = st.mic < 0 ? UI_COLOR_TEXT_DIM : st.mic ? UI_COLOR_GREEN : UI_COLOR_RED;
    lv_obj_t *mic_label = lv_obj_get_child(page->mic, 0);
    lv_label_set_text(mic_label, st.mic < 0 ? "Mic" : st.mic ? "Mic attivo" : "Mic muto");
    lv_obj_set_style_text_color(mic_label, ui_color(mic_color), 0);
    lv_obj_set_style_border_width(page->mic, 2, 0);
    lv_obj_set_style_border_color(page->mic, ui_color(st.mic < 0 ? UI_COLOR_SEPARATOR : mic_color), 0);
}

static void deleted(lv_event_t *e)
{
    deck_page_t *page = lv_event_get_user_data(e);
    lv_timer_delete(page->timer);
    if (s_page == page) s_page = NULL;
    heap_caps_free(page->mine);
    heap_caps_free(page->launch);
    heap_caps_free(page->git);
    heap_caps_free(page->ci);
    free(page);
}

/* The guide's button: install the agent when it is missing, otherwise switch the transport. */
static void guide_clicked(lv_event_t *e)
{
    if (s_page != NULL && s_page->view == VIEW_AGENT)
    {
        app_open_app(&s_manage_app, NULL);
        return;
    }
    remote_change_transport(e);
}

static void build_guide(deck_page_t *page, lv_obj_t *screen, int32_t h)
{
    page->guide = layer(screen);
    if (remote_bar_size(screen_w(), h))
    {
        int32_t w = screen_w();
        page->guide_icon = ui_icon_bubble(page->guide, LV_SYMBOL_BLUETOOTH, UI_COLOR_BLUE, 56);
        lv_obj_set_pos(page->guide_icon, 12, 70);
        page->guide_title = line(page->guide, 82, 26, w - 100, UI_FONT_BODY, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
        page->guide_text = line(page->guide, 82, 68, w - 290, UI_FONT_SMALL, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_height(page->guide_text, 84);
        lv_label_set_long_mode(page->guide_text, LV_LABEL_LONG_MODE_WRAP);
        page->guide_button = tile(page->guide, (ui_key_rect_t){w - 198, 80, 190, 64}, "", guide_clicked, NULL);
        return;
    }
    page->guide_icon = ui_icon_bubble(page->guide, LV_SYMBOL_BLUETOOTH, UI_COLOR_BLUE, 56);
    lv_obj_set_pos(page->guide_icon, screen_w() / 2 - 28, h * 16 / 100);
    ui_key_rect_t r = strip(h * 16 / 100 + 66, 34);
    page->guide_title = line(page->guide, r.x, r.y, r.w, UI_FONT_LARGE, UI_COLOR_TEXT, LV_TEXT_ALIGN_CENTER);
    r = strip(h * 16 / 100 + 106, h * 26 / 100);
    page->guide_text = lv_label_create(page->guide);
    lv_obj_set_pos(page->guide_text, r.x + 8, r.y);
    lv_obj_set_size(page->guide_text, r.w - 16, r.h);
    lv_obj_set_style_text_align(page->guide_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(page->guide_text, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(page->guide_text, ui_color(UI_COLOR_TEXT_DIM), 0);
    lv_label_set_long_mode(page->guide_text, LV_LABEL_LONG_MODE_WRAP);
    r = strip(h * 80 / 100, h * 12 / 100);
    page->guide_button = tile(page->guide, r, "", guide_clicked, NULL);
    lv_obj_set_style_border_color(page->guide_button, ui_accent(), 0);
}

static void build_bar(deck_page_t *page, lv_obj_t *screen, int32_t w, int32_t h)
{
    bool narrow = remote_narrow_size(w, h);
    page->per_page = 5;
    build_guide(page, screen, h);
    page->live = layer(screen);
    track_drags(page->live);
    page->icon = ui_icon_bubble(page->live, LV_SYMBOL_HOME, UI_COLOR_GRAY, 26);
    lv_obj_set_pos(page->icon, 62, 25);
    page->app = line(page->live, narrow ? 12 : 96, narrow ? 60 : 27, narrow ? w - 24 : w * 3 / 4 - 160, UI_FONT_BODY, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_t *header = lv_obj_create(page->live);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, narrow ? 8 : 58, narrow ? 56 : 22);
    lv_obj_set_size(header, narrow ? w - 16 : w * 3 / 4 - 120, 32);
    lv_obj_add_event_cb(header, header_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_move_background(header);
    lv_obj_t *pager = tile(page->live, (ui_key_rect_t){narrow ? w - 68 : w * 3 / 4 - 60, 22, 60, 32}, "", pager_clicked, NULL);
    page->pager = lv_obj_get_child(pager, 0);
    page->title = line(page->live, 12, narrow ? 84 : 55, narrow ? w - 24 : w * 3 / 4 - 20, UI_FONT_SMALL, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT);
    page->media = line(page->live, narrow ? 12 : w * 3 / 4 + 8, narrow ? h - 134 : 26, narrow ? w - 24 : w / 4 - 20, UI_FONT_SMALL, UI_COLOR_PINK, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_height(page->media, narrow ? 22 : 42);
    lv_label_set_long_mode(page->media, LV_LABEL_LONG_MODE_WRAP);
    for (int i = 0; i < 5; ++i)
    {
        page->keys[i] = tile(page->live, remote_bar_key(w, h, i), "", key_clicked, (void *)(intptr_t)i);
        track_drags(page->keys[i]);
    }
    const uint16_t usages[] = {HIDL_MEDIA_PREVIOUS, HIDL_MEDIA_PLAY_PAUSE, HIDL_MEDIA_NEXT};
    const char *symbols[] = {LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT};
    for (int i = 0; i < 3; ++i)
        tile(page->live, remote_bar_media(w, h, i), symbols[i], media_clicked, (void *)(intptr_t)usages[i]);
    page->mic = tile(page->live, remote_bar_media(w, h, 3), "Mic", action_clicked, "mic");
    remote_switcher_button(screen);
}

static void deck_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    deck_page_t *page = calloc(1, sizeof(*page));
    if (page == NULL)
        return;
    page->mine = heap_caps_calloc(DECK_LIST_MAX, sizeof(deck_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    page->launch = heap_caps_calloc(DECK_LIST_MAX, sizeof(deck_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    page->git = heap_caps_calloc(DECK_LIST_MAX, sizeof(deck_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    page->ci = heap_caps_calloc(DECK_LIST_MAX, sizeof(deck_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (page->mine == NULL || page->launch == NULL || page->git == NULL || page->ci == NULL)
    {
        heap_caps_free(page->mine);
        heap_caps_free(page->launch);
        heap_caps_free(page->git);
        heap_caps_free(page->ci);
        free(page);
        return;
    }
    page->mine_seen = page->launch_seen = page->git_seen = page->ci_seen = UINT32_MAX;
    s_page = page;
    page->seen = UINT32_MAX;
    lv_obj_set_scrollable(screen, false);
    lv_obj_add_event_cb(screen, page_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(screen, page_knob, lv_port_knob_event(), NULL);

    int32_t w = screen_w(), h = screen_h();
    if (remote_bar_size(w, h) || remote_narrow_size(w, h))
    {
        build_bar(page, screen, w, h);
        page->timer = lv_timer_create(tick, 150, page);
        lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, page);
        tick(page->timer);
        return;
    }
    bool round = board_info()->round;
    bool landscape = w > h + 40;
    int cols = landscape ? 3 : 2;
    int rows = landscape ? 3 : 4;
    page->per_page = cols * rows;

    build_guide(page, screen, h);

    /* Vertical budget: header, title, grid, media line, footer. Round panels
     * keep everything inside the central chords. */
    int32_t top = round ? h * 9 / 100 : REMOTE_TOP;
    int32_t bottom = round ? h * 91 / 100 : h - 6;
    int32_t head_h = 44, title_h = lv_font_get_line_height(UI_FONT_SMALL) + 4;
    int32_t foot_h = round ? 50 : 52, media_h = lv_font_get_line_height(UI_FONT_SMALL) + 4;
    int32_t grid_top = top + head_h + title_h;
    int32_t grid_h = bottom - foot_h - media_h - grid_top;

    page->live = layer(screen);
    track_drags(page->live);
    ui_key_rect_t head = strip(top, head_h);
    int32_t icon = head_h - 10;
    page->icon = ui_icon_bubble(page->live, LV_SYMBOL_HOME, UI_COLOR_GRAY, icon);
    lv_obj_remove_flag(page->icon, LV_OBJ_FLAG_CLICKABLE); /* taps reach the header below */
    lv_obj_set_pos(page->icon, head.x + 50, head.y + 5);
    int32_t name_x = head.x + 50 + icon + 8;
    page->app = line(page->live, name_x, head.y + (head_h - lv_font_get_line_height(UI_FONT_LARGE)) / 2,
                     head.x + head.w - 64 - name_x, UI_FONT_LARGE, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    /* The header names the section: a tap opens the index of sections. */
    lv_obj_t *header = lv_obj_create(page->live);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, head.x + 48, head.y);
    lv_obj_set_size(header, head.w - 48 - 62, head_h);
    lv_obj_set_style_radius(header, 12, 0);
    lv_obj_set_style_bg_color(header, ui_color(UI_COLOR_CARD_HI), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(header, header_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_move_background(header);
    lv_obj_t *pager = tile(page->live, (ui_key_rect_t){head.x + head.w - 60, head.y, 60, head_h}, "", pager_clicked,
                           NULL);
    lv_obj_set_style_border_width(pager, 0, 0);
    page->pager = lv_obj_get_child(pager, 0);
    lv_obj_set_style_text_color(page->pager, ui_color(UI_COLOR_TEXT_DIM), 0);

    ui_key_rect_t r = strip(top + head_h, title_h);
    page->title = line(page->live, r.x, r.y + 2, r.w, UI_FONT_SMALL, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_CENTER);

    for (int i = 0; i < page->per_page; i++)
    {
        int row = i / cols, col = i % cols;
        int32_t row_top = grid_top + grid_h * row / rows;
        r = strip(row_top, grid_h * (row + 1) / rows - grid_h * row / rows);
        int32_t x0 = r.x + r.w * col / cols, x1 = r.x + r.w * (col + 1) / cols;
        page->keys[i] = tile(page->live, (ui_key_rect_t){x0, r.y, x1 - x0, r.h}, "", key_clicked, (void *)(intptr_t)i);
        track_drags(page->keys[i]);
    }

    r = strip(bottom - foot_h - media_h, media_h);
    page->media = line(page->live, r.x, r.y + 2, r.w, UI_FONT_SMALL, UI_COLOR_PINK, LV_TEXT_ALIGN_CENTER);
    r = strip(bottom - foot_h, foot_h);
    static const uint16_t usages[] = {HIDL_MEDIA_PREVIOUS, HIDL_MEDIA_PLAY_PAUSE, HIDL_MEDIA_NEXT};
    static const char *const symbols[] = {LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT};
    for (int i = 0; i < 3; i++)
        tile(page->live, (ui_key_rect_t){r.x + r.w * i / 4, r.y, r.w * (i + 1) / 4 - r.w * i / 4, r.h}, symbols[i],
             media_clicked, (void *)(intptr_t)usages[i]);
    page->mic = tile(page->live, (ui_key_rect_t){r.x + r.w * 3 / 4, r.y, r.w - r.w * 3 / 4, r.h}, "Mic",
                     action_clicked, "mic");

    /* Switcher on top of both layers */
    remote_switcher_button(screen);

    page->timer = lv_timer_create(tick, 150, page);
    lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, page);
    tick(page->timer);
}

const app_t remote_deck_app = {.id = "remote.deck",
                               .name = "PC",
                               .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON | APP_FLAG_NO_BACK_GESTURE,
                               .create = deck_create};
