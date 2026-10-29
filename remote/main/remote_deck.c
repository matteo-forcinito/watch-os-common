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
 *   watch -> PC   {"t":"hello","v":0,"name":".."}
 *                 {"t":"act","a":"lock"|"mic"|"display_off"|"taskmgr"}
 */
#include "remote.h"
#include "core/lv_port.h"
#include "hardware/board.h"
#include "ui/ui.h"
#include "ui/keyboard_layout.h"

#include "freertos/FreeRTOS.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char app[24];
    char title[64];
    char track[64];
    char artist[48];
    char message[48];
    int8_t cpu, ram, mic; /* -1 unknown */
    bool playing;
    uint32_t version;
} deck_state_t;

static deck_state_t s_state = {.cpu = -1, .ram = -1, .mic = -1};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_hello_sent;

/* ---- minimal JSON field reader: flat objects, string or integer values ---- */

static const char *field(const char *json, const char *key)
{
    char pattern[16];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL)
        return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':')
        p++;
    return p;
}

static void text_field(const char *json, const char *key, char *out, size_t size)
{
    const char *p = field(json, key);
    size_t n = 0;
    if (p == NULL || *p != '"')
        return;
    for (p++; *p != '\0' && *p != '"' && n + 1 < size; p++)
    {
        if (*p == '\\' && p[1] != '\0')
            p++; /* \" and \\ only: the agent sends no other escapes */
        out[n++] = *p;
    }
    out[n] = '\0';
}

static int number_field(const char *json, const char *key, int fallback)
{
    const char *p = field(json, key);
    return p != NULL && (*p == '-' || (*p >= '0' && *p <= '9')) ? atoi(p) : fallback;
}

/* Bluetooth host task: copy into the shared state, the UI polls it. */
static void receive(const char *json, size_t length)
{
    (void)length;
    char type[8] = "";
    deck_state_t next;
    text_field(json, "t", type, sizeof(type));

    taskENTER_CRITICAL(&s_lock);
    next = s_state;
    taskEXIT_CRITICAL(&s_lock);

    if (strcmp(type, "ctx") == 0)
    {
        next.app[0] = next.title[0] = '\0';
        text_field(json, "app", next.app, sizeof(next.app));
        text_field(json, "title", next.title, sizeof(next.title));
    }
    else if (strcmp(type, "media") == 0)
    {
        next.track[0] = next.artist[0] = '\0';
        text_field(json, "title", next.track, sizeof(next.track));
        text_field(json, "artist", next.artist, sizeof(next.artist));
        next.playing = number_field(json, "playing", 0) != 0;
    }
    else if (strcmp(type, "sys") == 0)
    {
        next.cpu = (int8_t)number_field(json, "cpu", -1);
        next.ram = (int8_t)number_field(json, "ram", -1);
        next.mic = (int8_t)number_field(json, "mic", -1);
    }
    else if (strcmp(type, "res") == 0)
    {
        next.message[0] = '\0';
        text_field(json, "msg", next.message, sizeof(next.message));
    }
    else
    {
        return;
    }

    next.version++;
    taskENTER_CRITICAL(&s_lock);
    s_state = next;
    taskEXIT_CRITICAL(&s_lock);
}

void remote_deck_init(void)
{
    hid_deck_set_receiver(receive);
}

static void send_action(const char *action)
{
    char json[48];
    snprintf(json, sizeof(json), "{\"t\":\"act\",\"a\":\"%s\"}", action);
    if (!hid_deck_send(json))
        ui_toast("Avvia l'agente sul PC");
}

/* ---- contextual touchbar ---- */

#define CMD 0x80 /* Ctrl, or Cmd on macOS */

typedef struct
{
    const char *label;
    uint8_t modifiers;
    uint8_t key;
} deck_key_t;

typedef struct
{
    const char *match; /* comma separated process names, lower case, no .exe */
    const char *name;
    deck_key_t keys[4];
} deck_profile_t;

static const deck_profile_t s_profiles[] = {
    {"code,cursor,codium",
     "VS Code",
     {{"Comandi", CMD | HIDL_MOD_SHIFT, 0x13},
      {"Terminale", CMD, 0x35},
      {"Cerca file", CMD, 0x13},
      {"Cerca ovunque", CMD | HIDL_MOD_SHIFT, 0x09}}},
    {"chrome,msedge,firefox,brave,opera,vivaldi",
     "Browser",
     {{"Nuova scheda", CMD, 0x17},
      {"Chiudi scheda", CMD, 0x1A},
      {"Ricarica", 0, 0x3E},
      {"Riapri scheda", CMD | HIDL_MOD_SHIFT, 0x17}}},
    {"explorer,nautilus,dolphin,thunar",
     "File",
     {{"Nuova cartella", CMD | HIDL_MOD_SHIFT, 0x11},
      {"Rinomina", 0, 0x3B},
      {"Cartella su", HIDL_MOD_ALT, HIDL_KEY_UP},
      {"Cerca", CMD, 0x09}}},
    {"winword,soffice,excel,powerpnt",
     "Office",
     {{"Grassetto", CMD, 0x05}, {"Salva", CMD, 0x16}, {"Annulla", CMD, 0x1D}, {"Ripeti", CMD, 0x1C}}},
    {"windowsterminal,cmd,powershell,pwsh,wt,gnome-terminal,konsole,alacritty,wezterm",
     "Terminale",
     {{"Interrompi", HIDL_MOD_CTRL, 0x06},
      {"Pulisci", HIDL_MOD_CTRL, 0x0F},
      {"Copia", HIDL_MOD_CTRL | HIDL_MOD_SHIFT, 0x06},
      {"Incolla", HIDL_MOD_CTRL | HIDL_MOD_SHIFT, 0x19}}},
    {"spotify,vlc,musicbee,foobar2000",
     "Musica",
     {{"Brano prec.", HIDL_MOD_CTRL, HIDL_KEY_LEFT},
      {"Brano succ.", HIDL_MOD_CTRL, HIDL_KEY_RIGHT},
      {"Cerca", CMD, 0x0F},
      {"Mi piace", HIDL_MOD_ALT | HIDL_MOD_SHIFT, 0x0B}}},
    {"teams,ms-teams,zoom,discord,slack",
     "Chiamata",
     {{"Microfono", CMD | HIDL_MOD_SHIFT, 0x10},
      {"Video", CMD | HIDL_MOD_SHIFT, 0x0E},
      {"Mano", CMD | HIDL_MOD_SHIFT, 0x0D},
      {"Condividi", CMD | HIDL_MOD_SHIFT, 0x0E}}},
};

static const deck_profile_t s_generic = {
    "",
    "Generale",
    {{"Copia", CMD, 0x06}, {"Incolla", CMD, 0x19}, {"Annulla", CMD, 0x1D}, {"Cambia app", HIDL_MOD_ALT, HIDL_KEY_TAB}}};

static bool matches(const char *list, const char *app)
{
    size_t n = strlen(app);
    for (const char *p = list; n > 0 && *p != '\0';)
    {
        const char *end = strchr(p, ',');
        size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
        if (len == n && strncmp(p, app, n) == 0)
            return true;
        p += len + (end != NULL);
    }
    return false;
}

static const deck_profile_t *profile_for(const char *app)
{
    for (size_t i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++)
        if (matches(s_profiles[i].match, app))
            return &s_profiles[i];
    return &s_generic;
}

/* ---- page ---- */

typedef struct
{
    lv_obj_t *app, *detail, *media, *status, *mic;
    lv_obj_t *keys[4];
    lv_timer_t *timer;
    const deck_profile_t *profile;
    uint32_t seen;
    bool agent;
} deck_page_t;

static deck_page_t *s_page;

static lv_obj_t *button(lv_obj_t *parent, ui_key_rect_t r, const char *text, uint32_t color, lv_event_cb_t cb,
                        void *data)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, r.x + 3, r.y + 3);
    lv_obj_set_size(b, r.w - 6, r.h - 6);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, ui_color(color), 0);
    lv_obj_set_style_bg_color(b, ui_accent(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(b, false);
    lv_obj_set_gesture_bubble(b, false);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_port_set_focusable(b);
    lv_obj_t *label = lv_label_create(b);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, r.w - 12);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(label);
    return b;
}

static ui_key_rect_t row_rect(int row, int column, int columns)
{
    int32_t h = board_info()->height;
    ui_key_rect_t band =
        ui_keyboard_band(board_info()->width, h, board_info()->round, h * (40 + 15 * row) / 100, h * 14 / 100, 8);
    int32_t w = band.w / columns;
    return (ui_key_rect_t){band.x + w * column, band.y, column == columns - 1 ? band.w - w * column : w, band.h};
}

static lv_obj_t *line_label(lv_obj_t *screen, int32_t y_percent, const lv_font_t *font, uint32_t color)
{
    int32_t h = board_info()->height;
    ui_key_rect_t band = ui_keyboard_band(board_info()->width, h, board_info()->round, h * y_percent / 100, 24, 8);
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_pos(label, band.x, band.y);
    lv_obj_set_width(label, band.w);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, ui_color(color), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, "");
    return label;
}

static void key_clicked(lv_event_t *e)
{
    if (s_page == NULL)
        return;
    const deck_key_t *key = &s_page->profile->keys[(intptr_t)lv_event_get_user_data(e)];
    uint8_t mods = key->modifiers & ~CMD;
    if (key->modifiers & CMD)
        mods |= remote_settings()->host_os == 1 ? HIDL_MOD_GUI : HIDL_MOD_CTRL;
    remote_send_ok(hid_link_key(mods, key->key));
}

static void action_clicked(lv_event_t *e)
{
    send_action(lv_event_get_user_data(e));
}

static void media_clicked(lv_event_t *e)
{
    remote_send_ok(hid_link_media((uint16_t)(intptr_t)lv_event_get_user_data(e)));
}

static void set_profile(deck_page_t *page, const deck_profile_t *profile)
{
    if (page->profile == profile)
        return;
    page->profile = profile;
    for (int i = 0; i < 4; i++)
        lv_label_set_text(lv_obj_get_child(page->keys[i], 0), profile->keys[i].label);
}

static void tick(lv_timer_t *timer)
{
    deck_page_t *page = lv_timer_get_user_data(timer);
    bool agent = hid_deck_connected();

    if (agent && !s_hello_sent)
    {
        char hello[64];
        snprintf(hello, sizeof(hello), "{\"t\":\"hello\",\"v\":0,\"name\":\"%s\"}", remote_device_name());
        s_hello_sent = hid_deck_send(hello);
    }
    if (!agent)
        s_hello_sent = false;

    deck_state_t st;
    taskENTER_CRITICAL(&s_lock);
    st = s_state;
    taskEXIT_CRITICAL(&s_lock);

    if (st.version == page->seen && agent == page->agent)
        return;
    bool new_message = st.message[0] != '\0' && st.version != page->seen;
    page->seen = st.version;
    page->agent = agent;

    if (!agent)
    {
        set_profile(page, &s_generic);
        lv_label_set_text(page->app, hid_link_state() == HID_LINK_CONNECTED ? "Agente non attivo" : "Non connesso");
        lv_label_set_text(page->detail, "tools/deck_agent sul PC");
        lv_label_set_text(page->media, "");
        lv_label_set_text(page->status, remote_status_text());
        return;
    }

    const deck_profile_t *profile = profile_for(st.app);
    set_profile(page, profile);
    lv_label_set_text(page->app, profile == &s_generic && st.app[0] ? st.app : profile->name);
    lv_label_set_text(page->detail, st.title);
    if (st.track[0])
        lv_label_set_text_fmt(page->media, "%s %s%s%s", st.playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, st.track,
                              st.artist[0] ? " - " : "", st.artist);
    else
        lv_label_set_text(page->media, "");

    if (st.cpu >= 0)
        lv_label_set_text_fmt(page->status, "CPU %d%%  RAM %d%%", st.cpu, st.ram);
    lv_obj_set_style_bg_color(page->mic,
                              ui_color(st.mic == 0   ? UI_COLOR_RED
                                       : st.mic == 1 ? UI_COLOR_GREEN
                                                     : UI_COLOR_CARD),
                              0);
    if (new_message)
    {
        ui_toast(st.message);
        taskENTER_CRITICAL(&s_lock);
        s_state.message[0] = '\0';
        taskEXIT_CRITICAL(&s_lock);
    }
}

static void deleted(lv_event_t *e)
{
    (void)e;
    if (s_page != NULL)
    {
        lv_timer_delete(s_page->timer);
        free(s_page);
        s_page = NULL;
    }
}

static void back_clicked(lv_event_t *e)
{
    (void)e;
    app_back();
}

static void deck_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    deck_page_t *page = calloc(1, sizeof(*page));
    if (page == NULL)
        return;
    s_page = page;
    page->seen = UINT32_MAX;
    lv_obj_set_scrollable(screen, false);

    int32_t h = board_info()->height;
    ui_key_rect_t header = ui_keyboard_band(board_info()->width, h, board_info()->round, h * 9 / 100, h * 10 / 100, 8);
    button(screen, (ui_key_rect_t){header.x, header.y, 40, header.h}, LV_SYMBOL_LEFT, UI_COLOR_CARD, back_clicked,
           NULL);

    page->app = line_label(screen, 21, UI_FONT_LARGE, UI_COLOR_TEXT);
    page->detail = line_label(screen, 29, UI_FONT_SMALL, UI_COLOR_TEXT_DIM);
    page->media = line_label(screen, 34, UI_FONT_SMALL, UI_COLOR_PINK);

    for (int i = 0; i < 4; i++)
        page->keys[i] = button(screen, row_rect(i / 2, i % 2, 2), "", UI_COLOR_BLUE, key_clicked, (void *)(intptr_t)i);

    button(screen, row_rect(2, 0, 4), LV_SYMBOL_PREV, UI_COLOR_CARD, media_clicked,
           (void *)(intptr_t)HIDL_MEDIA_PREVIOUS);
    button(screen, row_rect(2, 1, 4), LV_SYMBOL_PLAY, UI_COLOR_PINK, media_clicked,
           (void *)(intptr_t)HIDL_MEDIA_PLAY_PAUSE);
    button(screen, row_rect(2, 2, 4), LV_SYMBOL_NEXT, UI_COLOR_CARD, media_clicked, (void *)(intptr_t)HIDL_MEDIA_NEXT);
    page->mic = button(screen, row_rect(2, 3, 4), "Mic", UI_COLOR_CARD, action_clicked, "mic");

    button(screen, row_rect(3, 0, 2), "Blocca PC", UI_COLOR_INDIGO, action_clicked, "lock");
    button(screen, row_rect(3, 1, 2), "Schermo off", UI_COLOR_CARD, action_clicked, "display_off");

    page->status = line_label(screen, 89, UI_FONT_SMALL, UI_COLOR_TEXT_DIM);
    page->profile = NULL;
    set_profile(page, &s_generic);
    page->timer = lv_timer_create(tick, 250, page);
    lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, NULL);
    tick(page->timer);
}

const app_t remote_deck_app = {.id = "remote.deck",
                               .name = "PC",
                               .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON | APP_FLAG_NO_BACK_GESTURE,
                               .create = deck_create};
