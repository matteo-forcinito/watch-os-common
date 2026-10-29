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
 *                 {"t":"ping"} every 2 s, {"t":"bye"} on exit: the watch shows the touchbar
 *                 only while the agent is heard
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
#include "freertos/task.h"

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
/* Windows restores a bonded host's notification subscription by itself, so a
 * subscription alone does not mean the agent runs: it must also have spoken
 * (any message, or its 2 s ping) within the last few seconds. */
static volatile TickType_t s_last_rx;
static volatile bool s_heard;
#define AGENT_TIMEOUT pdMS_TO_TICKS(6000)

static bool agent_alive(void)
{
    return hid_deck_connected() && s_heard && xTaskGetTickCount() - s_last_rx < AGENT_TIMEOUT;
}

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

    s_last_rx = xTaskGetTickCount();
    s_heard = strcmp(type, "bye") != 0; /* the agent says goodbye when it quits */
    if (strcmp(type, "ping") == 0 || strcmp(type, "bye") == 0)
        return;

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
    static unsigned id; /* the agent runs each id once, whatever the radio repeats */
    char json[64];
    snprintf(json, sizeof(json), "{\"t\":\"act\",\"a\":\"%s\",\"id\":%u}", action, ++id);
    if (!hid_deck_send(json))
        ui_toast("Avvia l'agente sul PC");
}

/* ---- profiles ----
 *
 * One profile per application (and per site, matched on the window title).
 * Keys are HID shortcuts sent by the watch; a key with an `action` asks the
 * agent instead (lock, display off, microphone). Letters and function keys
 * only: punctuation moves between host keyboard layouts. */

#define CMD 0x80 /* Ctrl, or Cmd on macOS */
#define C CMD
#define S HIDL_MOD_SHIFT
#define A HIDL_MOD_ALT
#define W HIDL_MOD_GUI

enum
{
    K_A = 0x04, K_B, K_C, K_D, K_E, K_F, K_G, K_H, K_I, K_J, K_K, K_L, K_M,
    K_N, K_O, K_P, K_Q, K_R, K_S, K_T, K_U, K_V, K_W, K_X, K_Y, K_Z,
    K_1 = 0x1E, K_2, K_3, K_4, K_5, K_6, K_0 = 0x27,
    K_ENTER = 0x28, K_ESC = 0x29, K_TAB = 0x2B, K_SPACE = 0x2C,
    K_F1 = 0x3A, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12,
    K_HOME = 0x4A, K_PGUP = 0x4B, K_DEL = 0x4C, K_END = 0x4D, K_PGDN = 0x4E,
    K_RIGHT = 0x4F, K_LEFT = 0x50, K_DOWN = 0x51, K_UP = 0x52,
    K_KP_MINUS = 0x56, K_KP_PLUS = 0x57,
};

typedef struct
{
    const char *label;
    uint8_t modifiers;
    uint8_t key;
    const char *action; /* agent action instead of a key, or NULL */
} deck_key_t;

typedef struct
{
    const char *apps;  /* comma separated process names, lower case, no .exe; NULL = any */
    const char *title; /* part of the window title; NULL = any */
    const char *name;
    const char *icon;
    uint32_t color;
    const deck_key_t *keys;
    uint8_t count;
} deck_profile_t;

/* Tables below list only the fields they use: `action` defaults to NULL. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#define KEYS(array) array, (uint8_t)(sizeof(array) / sizeof(array[0]))

static const deck_key_t k_youtube[] = {
    {"Play / pausa", 0, K_K},   {"Muto", 0, K_M},          {"-10 s", 0, K_J},         {"+10 s", 0, K_L},
    {"Schermo intero", 0, K_F}, {"Sottotitoli", 0, K_C},   {"Modalita cinema", 0, K_T}, {"Mini player", 0, K_I},
    {"Volume +", 0, K_UP},      {"Volume -", 0, K_DOWN},   {"Video successivo", S, K_N}, {"Dall'inizio", 0, K_HOME},
};
static const deck_key_t k_code[] = {
    {"Comandi", C | S, K_P},    {"Apri file", C, K_P},          {"Pannello", C, K_J},          {"Barra laterale", C, K_B},
    {"Cerca ovunque", C | S, K_F}, {"Esplora", C | S, K_E},     {"Git", C | S, K_G},           {"Salva", C, K_S},
    {"Avvia debug", 0, K_F5},   {"Ferma debug", S, K_F5},       {"Breakpoint", 0, K_F9},       {"Step over", 0, K_F10},
    {"Step into", 0, K_F11},    {"Definizione", 0, K_F12},      {"Rinomina", 0, K_F2},         {"Formatta", S | A, K_F},
};
static const deck_key_t k_browser[] = {
    {"Nuova scheda", C, K_T},   {"Chiudi scheda", C, K_W},   {"Riapri chiusa", C | S, K_T}, {"Ricarica", 0, K_F5},
    {"Indietro", A, K_LEFT},    {"Avanti", A, K_RIGHT},      {"Indirizzo", C, K_L},         {"Cerca in pagina", C, K_F},
    {"Scheda succ.", C, K_TAB}, {"Scheda prec.", C | S, K_TAB}, {"Zoom +", C, K_KP_PLUS},   {"Zoom -", C, K_KP_MINUS},
    {"Zoom 100%", C, K_0},      {"Preferito", C, K_D},       {"Download", C, K_J},          {"Schermo intero", 0, K_F11},
};
static const deck_key_t k_files[] = {
    {"Nuova cartella", C | S, K_N}, {"Rinomina", 0, K_F2},     {"Cartella su", A, K_UP},     {"Cerca", C, K_E},
    {"Indietro", A, K_LEFT},        {"Avanti", A, K_RIGHT},    {"Indirizzo", A, K_D},        {"Anteprima", A, K_P},
    {"Copia", C, K_C},              {"Incolla", C, K_V},       {"Taglia", C, K_X},           {"Elimina", 0, K_DEL},
    {"Seleziona tutto", C, K_A},    {"Annulla", C, K_Z},       {"Proprieta", A, K_ENTER},    {"Nuova finestra", C, K_N},
};
static const deck_key_t k_word[] = {
    {"Grassetto", C, K_B},       {"Corsivo", C, K_I},         {"Sottolineato", C, K_U},   {"Salva", C, K_S},
    {"Annulla", C, K_Z},         {"Ripeti", C, K_Y},          {"Trova", C, K_F},          {"Sostituisci", C, K_H},
    {"A sinistra", C, K_L},      {"Al centro", C, K_E},       {"A destra", C, K_R},       {"Giustifica", C, K_J},
    {"Seleziona tutto", C, K_A}, {"Stampa", C, K_P},          {"Nuovo", C, K_N},          {"Salva con nome", 0, K_F12},
};
static const deck_key_t k_excel[] = {
    {"Modifica cella", 0, K_F2}, {"Salva", C, K_S},            {"Annulla", C, K_Z},         {"Filtro", C | S, K_L},
    {"Riempi in basso", C, K_D}, {"Riempi a destra", C, K_R},  {"Tabella", C, K_T},         {"Grafico", 0, K_F11},
    {"Foglio succ.", C, K_PGDN}, {"Foglio prec.", C, K_PGUP},  {"Colonna", C, K_SPACE},     {"Riga", S, K_SPACE},
    {"Grassetto", C, K_B},       {"Trova", C, K_F},            {"Vai a", 0, K_F5},          {"Ricalcola", 0, K_F9},
};
static const deck_key_t k_powerpoint[] = {
    {"Presenta", 0, K_F5},      {"Da questa slide", S, K_F5}, {"Nuova slide", C, K_M},      {"Duplica", C, K_D},
    {"Grassetto", C, K_B},      {"Salva", C, K_S},            {"Annulla", C, K_Z},          {"Ripeti", C, K_Y},
    {"Raggruppa", C, K_G},      {"Separa", C | S, K_G},       {"Schermo nero", 0, K_B},     {"Fine", 0, K_ESC},
};
static const deck_key_t k_terminal[] = {
    {"Interrompi", HIDL_MOD_CTRL, K_C},           {"Pulisci", HIDL_MOD_CTRL, K_L},     {"Comando prec.", 0, K_UP},
    {"Comando succ.", 0, K_DOWN},                 {"Invio", 0, K_ENTER},               {"Completa", 0, K_TAB},
    {"Copia", HIDL_MOD_CTRL | S, K_C},            {"Incolla", HIDL_MOD_CTRL | S, K_V}, {"Nuova scheda", HIDL_MOD_CTRL | S, K_T},
    {"Chiudi scheda", HIDL_MOD_CTRL | S, K_W},    {"Cerca", HIDL_MOD_CTRL | S, K_F},   {"Cronologia", HIDL_MOD_CTRL, K_R},
    {"Inizio riga", 0, K_HOME},                   {"Fine riga", 0, K_END},             {"Esci", HIDL_MOD_CTRL, K_D},
    {"Annulla riga", HIDL_MOD_CTRL, K_U},
};
static const deck_key_t k_spotify[] = {
    {"Play / pausa", 0, K_SPACE}, {"Precedente", C, K_LEFT}, {"Successivo", C, K_RIGHT}, {"Mi piace", A | S, K_B},
    {"Volume +", C, K_UP},        {"Volume -", C, K_DOWN},   {"Casuale", C, K_S},        {"Ripeti", C, K_R},
    {"Cerca", C, K_K},            {"Indietro", A, K_LEFT},   {"Avanti", A, K_RIGHT},     {"Coda", A | S, K_Q},
};
static const deck_key_t k_teams[] = {
    {"Microfono", C | S, K_M},  {"Video", C | S, K_O},       {"Alza la mano", C | S, K_K}, {"Condividi", C | S, K_E},
    {"Esci", C | S, K_H},       {"Accetta audio", C | S, K_S}, {"Accetta video", C | S, K_A}, {"Rifiuta", C | S, K_D},
    {"Attivita", C, K_1},       {"Chat", C, K_2},            {"Calendario", C, K_4},       {"Chiamate", C, K_5},
    {"Cerca", C, K_E},          {"Nuova chat", C, K_N},      {"Sfondo", C | S, K_P},       {"Impostazioni", C, K_F2},
};
static const deck_key_t k_zoom[] = {
    {"Microfono", A, K_A},     {"Video", A, K_V},          {"Alza la mano", A, K_Y},  {"Condividi", A, K_S},
    {"Chat", A, K_H},          {"Partecipanti", A, K_U},   {"Registra", A, K_R},      {"Pausa reg.", A, K_P},
    {"Galleria", A, K_F2},     {"Relatore", A, K_F1},      {"Schermo intero", A, K_F}, {"Esci", A, K_Q},
};
static const deck_key_t k_discord[] = {
    {"Microfono", C | S, K_M},     {"Silenzia tutto", C | S, K_D}, {"Cerca", C, K_K},        {"Segna letti", S, K_ESC},
    {"Server prec.", C | A, K_UP}, {"Server succ.", C | A, K_DOWN}, {"Canale prec.", A, K_UP}, {"Canale succ.", A, K_DOWN},
};
static const deck_key_t k_generic[] = {
    {"Copia", C, K_C},         {"Incolla", C, K_V},       {"Taglia", C, K_X},          {"Annulla", C, K_Z},
    {"Ripeti", C, K_Y},        {"Seleziona tutto", C, K_A}, {"Cambia app", A, K_TAB},  {"Chiudi finestra", A, K_F4},
};
static const deck_key_t k_system[] = {
    {"Blocca PC", 0, 0, "lock"},     {"Schermo off", 0, 0, "display_off"}, {"Microfono", 0, 0, "mic"},
    {"Task manager", C | S, K_ESC},  {"Screenshot", W | S, K_S},          {"Desktop", W, K_D},
    {"Esplora file", W, K_E},        {"Impostazioni", W, K_I},            {"Appunti", W, K_V},
    {"Proietta", W, K_P},            {"Affianca sx", W, K_LEFT},          {"Affianca dx", W, K_RIGHT},
    {"Massimizza", W, K_UP},         {"Riduci", W, K_DOWN},               {"Desktop succ.", W | HIDL_MOD_CTRL, K_RIGHT},
    {"Desktop prec.", W | HIDL_MOD_CTRL, K_LEFT},
};

#define BROWSERS "chrome,msedge,firefox,brave,opera,vivaldi,chromium"

/* First match wins: site profiles come before their browser. */
static const deck_profile_t s_profiles[] = {
    {BROWSERS, "YouTube", "YouTube", LV_SYMBOL_VIDEO, UI_COLOR_RED, KEYS(k_youtube)},
    {"code,cursor,codium,windsurf", NULL, "VS Code", LV_SYMBOL_EDIT, UI_COLOR_BLUE, KEYS(k_code)},
    {BROWSERS, NULL, "Browser", LV_SYMBOL_DIRECTORY, UI_COLOR_TEAL, KEYS(k_browser)},
    {"explorer,nautilus,dolphin,thunar,nemo", NULL, "File", LV_SYMBOL_DIRECTORY, UI_COLOR_ORANGE, KEYS(k_files)},
    {"winword", NULL, "Word", LV_SYMBOL_FILE, UI_COLOR_BLUE, KEYS(k_word)},
    {"excel", NULL, "Excel", LV_SYMBOL_LIST, UI_COLOR_GREEN, KEYS(k_excel)},
    {"powerpnt", NULL, "PowerPoint", LV_SYMBOL_IMAGE, UI_COLOR_ORANGE, KEYS(k_powerpoint)},
    {"soffice", NULL, "LibreOffice", LV_SYMBOL_FILE, UI_COLOR_BLUE, KEYS(k_word)},
    {"windowsterminal,cmd,powershell,pwsh,wt,conhost,gnome-terminal,konsole,alacritty,wezterm,kitty",
     NULL, "Terminale", LV_SYMBOL_KEYBOARD, UI_COLOR_GREEN, KEYS(k_terminal)},
    {"spotify", NULL, "Spotify", LV_SYMBOL_AUDIO, UI_COLOR_GREEN, KEYS(k_spotify)},
    {"ms-teams,teams", NULL, "Teams", LV_SYMBOL_CALL, UI_COLOR_INDIGO, KEYS(k_teams)},
    {"zoom", NULL, "Zoom", LV_SYMBOL_CALL, UI_COLOR_BLUE, KEYS(k_zoom)},
    {"discord", NULL, "Discord", LV_SYMBOL_CALL, UI_COLOR_INDIGO, KEYS(k_discord)},
};

static const deck_profile_t s_generic = {NULL, NULL, "", LV_SYMBOL_HOME, UI_COLOR_GRAY, KEYS(k_generic)};
#pragma GCC diagnostic pop

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
 *   header      back, profile icon and name, page n/m (tap for the next page)
 *               window title
 *   grid        keys of the focused application; swipe or knob for more pages.
 *               The last pages are always Sistema (lock, display, desktop, snap...)
 *   footer      what is playing (or CPU/RAM); previous, play, next, microphone
 *
 * Geometry follows the screen as LVGL draws it (rotation included):
 * landscape 3x3 keys, portrait 2x4, round panels 2x4 on circle chords.
 * Without the agent the page is a connection guide (USB, pairing, agent). */

#define GRID_MAX 9

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
    lv_obj_t *live, *icon, *app, *pager, *title, *keys[GRID_MAX], *media, *mic;
    lv_timer_t *timer;
    const deck_profile_t *profile;
    char app_name[24];
    int page, per_page;
    deck_view_t view;
    uint32_t seen;
} deck_page_t;

static deck_page_t *s_page;

#define SYSTEM_COUNT ((int)(sizeof(k_system) / sizeof(k_system[0])))

static int pages_of(int count, int per)
{
    return (count + per - 1) / per;
}

static int page_count(const deck_page_t *page)
{
    return pages_of(page->profile->count, page->per_page) + pages_of(SYSTEM_COUNT, page->per_page);
}

static bool system_page(const deck_page_t *page)
{
    return page->page >= pages_of(page->profile->count, page->per_page);
}

/* The key in slot `slot` of the current page, or NULL for an empty slot. */
static const deck_key_t *key_at(const deck_page_t *page, int slot)
{
    const deck_profile_t *p = page->profile;
    int own = pages_of(p->count, page->per_page);
    if (page->page < own)
    {
        int i = page->page * page->per_page + slot;
        return i < p->count ? &p->keys[i] : NULL;
    }
    int i = (page->page - own) * page->per_page + slot;
    return i < SYSTEM_COUNT ? &k_system[i] : NULL;
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
    bool system = system_page(page);

    for (int i = 0; i < page->per_page; i++)
    {
        const deck_key_t *key = key_at(page, i);
        if (key == NULL)
        {
            lv_obj_add_flag(page->keys[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(page->keys[i], LV_OBJ_FLAG_HIDDEN);
        fit_label(lv_obj_get_child(page->keys[i], 0), key->label);
        lv_obj_set_style_border_color(page->keys[i], ui_color(system ? UI_COLOR_SEPARATOR : p->color), 0);
        lv_obj_set_style_border_width(page->keys[i], system ? 1 : 2, 0);
    }

    lv_label_set_text(page->app, system ? "Sistema" : (p->name[0] ? p->name : page->app_name));
    lv_label_set_text(lv_obj_get_child(page->icon, 0), system ? LV_SYMBOL_SETTINGS : p->icon);
    ui_icon_bubble_set_color(page->icon, system ? UI_COLOR_GRAY : p->color);
    lv_label_set_text_fmt(page->pager, "%d/%d", page->page + 1, page_count(page));
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

static void key_clicked(lv_event_t *e)
{
    if (s_dragged)
    {
        s_dragged = false;
        return;
    }
    if (s_page == NULL || s_page->profile == NULL)
        return;
    const deck_key_t *key = key_at(s_page, (int)(intptr_t)lv_event_get_user_data(e));
    if (key == NULL)
        return;
    if (key->action != NULL)
    {
        send_action(key->action);
        return;
    }
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
        [VIEW_USB] = {LV_SYMBOL_USB, "Serve il Bluetooth",
                      "Via USB il PC riceve i tasti ma non puo dire al watch quale app e aperta.", UI_COLOR_ORANGE},
        [VIEW_PAIR] = {LV_SYMBOL_BLUETOOTH, "Associa il PC",
                       "Bluetooth del PC > Aggiungi dispositivo. Se era gia associato, rimuovilo e riassocialo.",
                       UI_COLOR_BLUE},
        [VIEW_AGENT] = {LV_SYMBOL_REFRESH, "Avvia l'agente",
                        "Sul PC esegui install_windows.ps1 in watch-os-common/tools/deck_agent.", UI_COLOR_GREEN},
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
    bool agent = agent_alive();

    if (agent && !s_hello_sent)
    {
        char hello[64];
        snprintf(hello, sizeof(hello), "{\"t\":\"hello\",\"v\":1,\"name\":\"%s\"}", remote_device_name());
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

static void build_guide(deck_page_t *page, lv_obj_t *screen, int32_t h)
{
    page->guide = layer(screen);
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
    page->guide_button = tile(page->guide, r, "Passa a Bluetooth", remote_change_transport, NULL);
    lv_obj_set_style_border_color(page->guide_button, ui_accent(), 0);
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
    lv_obj_add_event_cb(screen, page_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(screen, page_knob, lv_port_knob_event(), NULL);

    int32_t w = screen_w(), h = screen_h();
    bool round = board_info()->round;
    bool landscape = w > h + 40;
    int cols = landscape ? 3 : 2;
    int rows = landscape ? 3 : 4;
    page->per_page = cols * rows;

    build_guide(page, screen, h);

    /* Vertical budget: header, title, grid, media line, footer. Round panels
     * keep everything inside the central chords. */
    int32_t top = round ? h * 9 / 100 : 6;
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
    lv_obj_set_pos(page->icon, head.x + 50, head.y + 5);
    int32_t name_x = head.x + 50 + icon + 8;
    page->app = line(page->live, name_x, head.y + (head_h - lv_font_get_line_height(UI_FONT_LARGE)) / 2,
                     head.x + head.w - 64 - name_x, UI_FONT_LARGE, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
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

    /* Back on top of both layers */
    lv_obj_t *back = tile(screen, (ui_key_rect_t){head.x, head.y, 48, head_h}, LV_SYMBOL_LEFT, back_clicked, NULL);
    lv_obj_set_style_border_width(back, 0, 0);

    page->timer = lv_timer_create(tick, 150, page);
    lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, NULL);
    tick(page->timer);
}

const app_t remote_deck_app = {.id = "remote.deck",
                               .name = "PC",
                               .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON | APP_FLAG_NO_BACK_GESTURE,
                               .create = deck_create};
