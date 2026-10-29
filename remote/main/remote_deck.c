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
 *                 {"t":"prof","n":"VS Code"}            touchbar chosen for the last ctx
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

static const char *profile_name(const char *app, const char *title);

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
        /* Tell the agent which touchbar the watch chose (log and self-test). */
        char ack[64];
        snprintf(ack, sizeof(ack), "{\"t\":\"prof\",\"n\":\"%s\"}", profile_name(next.app, next.title));
        hid_deck_send(ack);
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
#define CS (CMD | HIDL_MOD_SHIFT)

typedef struct
{
    const char *label;
    uint8_t modifiers;
    uint8_t key;
} deck_key_t;

typedef struct
{
    const char *apps;  /* comma separated process names, lower case, no .exe; NULL = any */
    const char *title; /* case-sensitive part of the window title; NULL = any */
    const char *name;
    const char *icon;
    uint32_t color;
    deck_key_t keys[6];
} deck_profile_t;

#define BROWSERS "chrome,msedge,firefox,brave,opera,vivaldi,chromium"

/* First match wins: title-specific profiles come before their application. */
static const deck_profile_t s_profiles[] = {
    {BROWSERS,
     "YouTube",
     "YouTube",
     LV_SYMBOL_VIDEO,
     UI_COLOR_RED,
     {{"Play / pausa", 0, 0x0E},
      {"Muto", 0, 0x10},
      {"-10 s", 0, 0x0D},
      {"+10 s", 0, 0x0F},
      {"Schermo intero", 0, 0x09},
      {"Sottotitoli", 0, 0x06}}},
    {"code,cursor,codium,windsurf",
     NULL,
     "VS Code",
     LV_SYMBOL_EDIT,
     UI_COLOR_BLUE,
     {{"Comandi", CS, 0x13},
      {"Apri file", CMD, 0x13},
      {"Terminale", CMD, 0x35},
      {"Cerca ovunque", CS, 0x09},
      {"Formatta", HIDL_MOD_SHIFT | HIDL_MOD_ALT, 0x09},
      {"Salva", CMD, 0x16}}},
    {BROWSERS,
     NULL,
     "Browser",
     LV_SYMBOL_DIRECTORY,
     UI_COLOR_TEAL,
     {{"Nuova scheda", CMD, 0x17},
      {"Chiudi scheda", CMD, 0x1A},
      {"Riapri chiusa", CS, 0x17},
      {"Ricarica", 0, 0x3E},
      {"Indietro", HIDL_MOD_ALT, HIDL_KEY_LEFT},
      {"Indirizzo", CMD, 0x0F}}},
    {"explorer,nautilus,dolphin,thunar,nemo",
     NULL,
     "File",
     LV_SYMBOL_DIRECTORY,
     UI_COLOR_ORANGE,
     {{"Nuova cartella", CS, 0x11},
      {"Rinomina", 0, 0x3B},
      {"Cartella su", HIDL_MOD_ALT, HIDL_KEY_UP},
      {"Cerca", CMD, 0x09},
      {"Proprieta", HIDL_MOD_ALT, HIDL_KEY_ENTER},
      {"Nuova finestra", CMD, 0x11}}},
    {"winword,excel,powerpnt,soffice,onenote",
     NULL,
     "Office",
     LV_SYMBOL_FILE,
     UI_COLOR_BLUE,
     {{"Grassetto", CMD, 0x05},
      {"Corsivo", CMD, 0x0C},
      {"Sottolineato", CMD, 0x18},
      {"Salva", CMD, 0x16},
      {"Annulla", CMD, 0x1D},
      {"Ripeti", CMD, 0x1C}}},
    {"windowsterminal,cmd,powershell,pwsh,wt,conhost,gnome-terminal,konsole,alacritty,wezterm,kitty",
     NULL,
     "Terminale",
     LV_SYMBOL_KEYBOARD,
     UI_COLOR_GREEN,
     {{"Interrompi", HIDL_MOD_CTRL, 0x06},
      {"Pulisci", HIDL_MOD_CTRL, 0x0F},
      {"Comando prec.", 0, HIDL_KEY_UP},
      {"Invio", 0, HIDL_KEY_ENTER},
      {"Copia", HIDL_MOD_CTRL | HIDL_MOD_SHIFT, 0x06},
      {"Incolla", HIDL_MOD_CTRL | HIDL_MOD_SHIFT, 0x19}}},
    {"spotify",
     NULL,
     "Spotify",
     LV_SYMBOL_AUDIO,
     UI_COLOR_GREEN,
     {{"Precedente", HIDL_MOD_CTRL, HIDL_KEY_LEFT},
      {"Successivo", HIDL_MOD_CTRL, HIDL_KEY_RIGHT},
      {"Casuale", HIDL_MOD_CTRL, 0x16},
      {"Ripeti", HIDL_MOD_CTRL, 0x15},
      {"Cerca", HIDL_MOD_CTRL, 0x0F},
      {"Mi piace", HIDL_MOD_ALT | HIDL_MOD_SHIFT, 0x05}}},
    {"ms-teams,teams",
     NULL,
     "Teams",
     LV_SYMBOL_CALL,
     UI_COLOR_INDIGO,
     {{"Microfono", CS, 0x10},
      {"Video", CS, 0x12},
      {"Alza la mano", CS, 0x0E},
      {"Condividi", CS, 0x08},
      {"Chat", CMD, 0x06},
      {"Esci", CS, 0x0B}}},
    {"zoom",
     NULL,
     "Zoom",
     LV_SYMBOL_CALL,
     UI_COLOR_BLUE,
     {{"Microfono", HIDL_MOD_ALT, 0x04},
      {"Video", HIDL_MOD_ALT, 0x19},
      {"Alza la mano", HIDL_MOD_ALT, 0x1C},
      {"Condividi", HIDL_MOD_ALT, 0x16},
      {"Chat", HIDL_MOD_ALT, 0x0B},
      {"Esci", HIDL_MOD_ALT, 0x14}}},
    {"discord",
     NULL,
     "Discord",
     LV_SYMBOL_CALL,
     UI_COLOR_INDIGO,
     {{"Microfono", CS, 0x10},
      {"Silenzia tutto", CS, 0x07},
      {"Cerca", CMD, 0x0E},
      {"Server prec.", CMD | HIDL_MOD_ALT, HIDL_KEY_UP},
      {"Server succ.", CMD | HIDL_MOD_ALT, HIDL_KEY_DOWN},
      {"Non letti", HIDL_MOD_SHIFT, HIDL_KEY_ESC}}},
};

static const deck_profile_t s_generic = {NULL,
                                         NULL,
                                         "",
                                         LV_SYMBOL_HOME,
                                         UI_COLOR_GRAY,
                                         {{"Copia", CMD, 0x06},
                                          {"Incolla", CMD, 0x19},
                                          {"Taglia", CMD, 0x1B},
                                          {"Annulla", CMD, 0x1D},
                                          {"Cambia app", HIDL_MOD_ALT, HIDL_KEY_TAB},
                                          {"Desktop", HIDL_MOD_GUI, 0x07}}};

static bool in_list(const char *list, const char *app)
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

static const deck_profile_t *profile_for(const char *app, const char *title)
{
    for (size_t i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++)
    {
        const deck_profile_t *p = &s_profiles[i];
        if ((p->apps == NULL || in_list(p->apps, app)) && (p->title == NULL || strstr(title, p->title) != NULL))
            return p;
    }
    return &s_generic;
}

static const char *profile_name(const char *app, const char *title)
{
    const deck_profile_t *p = profile_for(app, title);
    return p->name[0] ? p->name : "Generale";
}

/* ---- page ----
 *
 * Without the agent the page is a connection guide (USB, pairing, agent):
 * the touchbar appears only when the PC really reports its context. */

typedef enum
{
    VIEW_NONE,
    VIEW_USB,
    VIEW_PAIR,
    VIEW_AGENT,
    VIEW_LIVE,
} deck_view_t;

typedef struct
{
    lv_obj_t *guide, *guide_icon, *guide_title, *guide_text, *guide_button;
    lv_obj_t *live, *icon, *app, *title, *keys[6], *media, *mic, *stats;
    lv_timer_t *timer;
    const deck_profile_t *profile;
    deck_view_t view;
    uint32_t seen;
} deck_page_t;

static deck_page_t *s_page;

static ui_key_rect_t band(int32_t top_percent, int32_t height_percent)
{
    int32_t h = board_info()->height;
    return ui_keyboard_band(board_info()->width, h, board_info()->round, h * top_percent / 100,
                            h * height_percent / 100, 8);
}

static lv_obj_t *tile(lv_obj_t *parent, ui_key_rect_t r, const char *text, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, r.x + 3, r.y + 3);
    lv_obj_set_size(b, r.w - 6, r.h - 6);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, ui_color(UI_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, ui_accent(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, ui_color(UI_COLOR_SEPARATOR), 0);
    lv_obj_set_scrollable(b, false);
    lv_obj_set_gesture_bubble(b, false);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_port_set_focusable(b);
    lv_obj_t *label = lv_label_create(b);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, r.w - 14);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(label, ui_color(UI_COLOR_TEXT), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(label);
    return b;
}

static lv_obj_t *text_at(lv_obj_t *parent, ui_key_rect_t r, const lv_font_t *font, uint32_t color, bool wrap)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, r.x, r.y);
    lv_obj_set_width(label, r.w);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, ui_color(color), 0);
    lv_label_set_long_mode(label, wrap ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, "");
    return label;
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

static void key_clicked(lv_event_t *e)
{
    if (s_page == NULL || s_page->profile == NULL)
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

static void set_profile(deck_page_t *page, const deck_profile_t *profile, const char *app)
{
    if (page->profile != profile)
    {
        page->profile = profile;
        ui_icon_bubble_set_color(page->icon, profile->color);
        lv_label_set_text(lv_obj_get_child(page->icon, 0), profile->icon);
        for (int i = 0; i < 6; i++)
        {
            lv_label_set_text(lv_obj_get_child(page->keys[i], 0), profile->keys[i].label);
            lv_obj_set_style_border_color(page->keys[i], ui_color(profile->color), 0);
        }
    }
    lv_label_set_text(page->app, profile->name[0] ? profile->name : (app[0] ? app : "PC"));
}

static void show_guide(deck_page_t *page, deck_view_t view)
{
    static const struct
    {
        const char *icon, *title, *text;
        uint32_t color;
    } guides[] = {
        [VIEW_USB] = {LV_SYMBOL_USB, "Deck usa il Bluetooth",
                      "Remote ora e collegata via USB: il PC riceve i tasti ma non puo inviare il contesto.",
                      UI_COLOR_ORANGE},
        [VIEW_PAIR] = {LV_SYMBOL_BLUETOOTH, "Associa il PC",
                       "Impostazioni Bluetooth del PC > Aggiungi dispositivo. Se era gia associato, rimuovilo e "
                       "riassocialo.",
                       UI_COLOR_BLUE},
        [VIEW_AGENT] = {LV_SYMBOL_REFRESH, "Avvia l'agente",
                        "Sul PC: python deck_agent.py\n(watch-os-common/tools/deck_agent).\nI tasti seguiranno "
                        "l'app aperta.",
                        UI_COLOR_GREEN},
    };
    ui_icon_bubble_set_color(page->guide_icon, guides[view].color);
    lv_label_set_text(lv_obj_get_child(page->guide_icon, 0), guides[view].icon);
    lv_label_set_text(page->guide_title, guides[view].title);
    lv_label_set_text(page->guide_text, guides[view].text);
    if (view == VIEW_USB)
        lv_obj_remove_flag(page->guide_button, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(page->guide_button, LV_OBJ_FLAG_HIDDEN);
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

    deck_view_t view = remote_settings()->transport == HID_LINK_USB ? VIEW_USB
                       : hid_link_state() != HID_LINK_CONNECTED    ? VIEW_PAIR
                       : !agent                                    ? VIEW_AGENT
                                                                   : VIEW_LIVE;
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
    taskENTER_CRITICAL(&s_lock);
    st = s_state;
    s_state.message[0] = '\0';
    taskEXIT_CRITICAL(&s_lock);

    if (st.message[0])
        ui_toast(st.message);
    if (st.version == page->seen)
        return;
    page->seen = st.version;

    set_profile(page, profile_for(st.app, st.title), st.app);
    lv_label_set_text(page->title, st.title);

    if (st.track[0])
        lv_label_set_text_fmt(page->media, "%s  %s%s%s", st.playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, st.track,
                              st.artist[0] ? " - " : "", st.artist);
    else
        lv_label_set_text(page->media, "Nessuna riproduzione");

    lv_obj_t *mic_label = lv_obj_get_child(page->mic, 0);
    if (st.mic < 0)
    {
        lv_label_set_text(mic_label, "Mic ?");
        lv_obj_set_style_border_color(page->mic, ui_color(UI_COLOR_SEPARATOR), 0);
    }
    else
    {
        lv_label_set_text(mic_label, st.mic ? LV_SYMBOL_AUDIO " Attivo" : LV_SYMBOL_MUTE " Muto");
        lv_obj_set_style_border_color(page->mic, ui_color(st.mic ? UI_COLOR_GREEN : UI_COLOR_RED), 0);
    }

    if (st.cpu >= 0)
        lv_label_set_text_fmt(page->stats, "CPU %d%%   RAM %d%%", st.cpu, st.ram);
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

    /* Connection guide */
    page->guide = layer(screen);
    ui_key_rect_t r = band(22, 16);
    page->guide_icon = ui_icon_bubble(page->guide, LV_SYMBOL_BLUETOOTH, UI_COLOR_BLUE, 64);
    lv_obj_set_pos(page->guide_icon, r.x + r.w / 2 - 32, r.y);
    page->guide_title = text_at(page->guide, band(40, 8), UI_FONT_LARGE, UI_COLOR_TEXT, false);
    page->guide_text = text_at(page->guide, band(50, 24), UI_FONT_BODY, UI_COLOR_TEXT_DIM, true);
    page->guide_button = tile(page->guide, band(76, 11), "Passa a Bluetooth", remote_change_transport, NULL);
    lv_obj_set_style_border_color(page->guide_button, ui_accent(), 0);

    /* Live touchbar */
    page->live = layer(screen);
    r = band(8, 10);
    page->icon = ui_icon_bubble(page->live, LV_SYMBOL_HOME, UI_COLOR_GRAY, 40);
    lv_obj_set_pos(page->icon, r.x + 56, r.y + (r.h - 40) / 2);
    page->app = text_at(page->live, (ui_key_rect_t){r.x + 104, r.y + (r.h - 30) / 2, r.w - 104, 30}, UI_FONT_LARGE,
                        UI_COLOR_TEXT, false);
    lv_obj_set_style_text_align(page->app, LV_TEXT_ALIGN_LEFT, 0);
    page->title = text_at(page->live, band(19, 5), UI_FONT_SMALL, UI_COLOR_TEXT_DIM, false);

    for (int i = 0; i < 6; i++)
    {
        r = band(25 + 13 * (i / 2), 13);
        int32_t half = r.w / 2;
        page->keys[i] = tile(page->live, (ui_key_rect_t){r.x + (i % 2 ? half : 0), r.y, i % 2 ? r.w - half : half, r.h},
                             "", key_clicked, (void *)(intptr_t)i);
    }

    page->media = text_at(page->live, band(65, 5), UI_FONT_SMALL, UI_COLOR_PINK, false);
    r = band(70, 11);
    static const uint16_t usages[] = {HIDL_MEDIA_PREVIOUS, HIDL_MEDIA_PLAY_PAUSE, HIDL_MEDIA_NEXT};
    static const char *const symbols[] = {LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT};
    int32_t third = r.w * 2 / 9;
    for (int i = 0; i < 3; i++)
        tile(page->live, (ui_key_rect_t){r.x + third * i, r.y, third, r.h}, symbols[i], media_clicked,
             (void *)(intptr_t)usages[i]);
    page->mic =
        tile(page->live, (ui_key_rect_t){r.x + third * 3, r.y, r.w - third * 3, r.h}, "Mic ?", action_clicked, "mic");

    r = band(81, 9);
    int32_t half = r.w / 2;
    tile(page->live, (ui_key_rect_t){r.x, r.y, half, r.h}, LV_SYMBOL_EYE_CLOSE " Blocca", action_clicked, "lock");
    tile(page->live, (ui_key_rect_t){r.x + half, r.y, r.w - half, r.h}, LV_SYMBOL_POWER " Schermo", action_clicked,
         "display_off");
    page->stats = text_at(page->live, band(91, 5), UI_FONT_SMALL, UI_COLOR_TEXT_DIM, false);

    /* Back on top of both layers */
    ui_key_rect_t header = band(8, 10);
    lv_obj_t *back = tile(screen, (ui_key_rect_t){header.x, header.y, 52, header.h}, LV_SYMBOL_LEFT, back_clicked, NULL);
    lv_obj_set_style_border_width(back, 0, 0);

    page->timer = lv_timer_create(tick, 200, page);
    lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, NULL);
    tick(page->timer);
}

const app_t remote_deck_app = {.id = "remote.deck",
                               .name = "PC",
                               .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON | APP_FLAG_NO_BACK_GESTURE,
                               .create = deck_create};
