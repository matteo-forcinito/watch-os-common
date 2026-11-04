/*
 * Deck core: the PC touchbar's data, shared by Remote's PC page and by devices
 * whose main screen is a touchbar (Deck Bar).
 *
 *   profiles   one per application (and per site), keys as HID shortcuts
 *   state      what the PC agent reported: focused app, media, CPU/RAM, mic
 *   protocol   JSON lines on hid_link's Deck channel (GATT on Bluetooth,
 *              vendor HID report on USB); see remote/main/remote_deck.c
 *
 * The link task writes the state, the UI copies it with deck_state_take().
 */

#include "deck_core/deck_core.h"

#include "hid_link/hid_link.h"
#include "ui/ui.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void (*s_command)(const char *json);

static deck_state_t s_state = {.cpu = -1, .ram = -1, .mic = -1};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
/* Agent lists (deck_list_t), in PSRAM: internal RAM is short with Bluetooth on. Under s_lock. */
typedef struct
{
    deck_item_t items[DECK_LIST_MAX];
    int count;
    uint32_t version;
    bool open; /* an item 0 arrived since the last end mark */
} list_t;
static list_t *s_lists;
/* Windows restores a bonded host's notification subscription by itself, so a
 * subscription alone does not mean the agent runs: it must also have spoken
 * (any message, or its 2 s ping) within the last few seconds. */
static volatile TickType_t s_last_rx;
static volatile bool s_heard;
static char s_agent_version[16]; /* reported by the agent ({"t":"agent","v":...}); under s_lock */
#define AGENT_TIMEOUT pdMS_TO_TICKS(15000) /* pings every 2 s; one slow GATT write may hold one for 5 s */

bool deck_agent_alive(void)
{
    return hid_deck_connected() && s_heard && xTaskGetTickCount() - s_last_rx < AGENT_TIMEOUT;
}

void deck_agent_version(char *out, size_t size)
{
    taskENTER_CRITICAL(&s_lock);
    snprintf(out, size, "%s", s_agent_version);
    taskEXIT_CRITICAL(&s_lock);
}

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

/* Keep only whole, valid UTF-8 sequences. A character cut by a length limit makes
 * LVGL's label renderer loop forever (the watch then resets on the LVGL stall). */
static void utf8_clean(char *text)
{
    unsigned char *r = (unsigned char *)text, *w = r;
    while (*r)
    {
        int n = *r < 0x80 ? 1 : (*r & 0xE0) == 0xC0 ? 2 : (*r & 0xF0) == 0xE0 ? 3 : (*r & 0xF8) == 0xF0 ? 4 : 0;
        int ok = n > 0 && !(n == 2 && *r < 0xC2);
        for (int k = 1; ok && k < n; k++)
            ok = (r[k] & 0xC0) == 0x80;
        if (!ok)
        {
            r++; /* drop the stray byte, resync on the next one */
            continue;
        }
        for (int k = 0; k < n; k++)
            *w++ = *r++;
    }
    *w = '\0';
}

void deck_json_text(const char *json, const char *key, char *out, size_t size)
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
    utf8_clean(out);
}

int deck_json_number(const char *json, const char *key, int fallback)
{
    const char *p = field(json, key);
    return p != NULL && (*p == '-' || (*p >= '0' && *p <= '9')) ? atoi(p) : fallback;
}

/* Link task (Bluetooth host or USB): copy into the shared state, the UI polls it. */
static void receive(const char *json, size_t length)
{
    (void)length;
    char type[8] = "";
    deck_state_t next;
    deck_json_text(json, "t", type, sizeof(type));

    s_last_rx = xTaskGetTickCount();
    s_heard = strcmp(type, "bye") != 0; /* the agent says goodbye when it quits */
    if (strcmp(type, "ping") == 0 || strcmp(type, "bye") == 0)
        return;
    if (strcmp(type, "cmd") == 0)
    {
        if (s_command != NULL)
            s_command(json);
        return;
    }
    int kind = strcmp(type, "win") == 0 ? DECK_LIST_WINDOWS
               : strcmp(type, "app") == 0 ? DECK_LIST_APPS
               : strcmp(type, "ck") == 0  ? DECK_LIST_KEYS
               : strcmp(type, "git") == 0 ? DECK_LIST_GIT
               : strcmp(type, "ci") == 0  ? DECK_LIST_CI
                                          : -1;
    if (kind >= 0)
    {
        /* One message per item, then i = -1: the list is complete (an empty list is just the end). */
        int i = deck_json_number(json, "i", -1);
        deck_item_t item = {0};
        deck_json_text(json, "a", item.app, sizeof(item.app));
        deck_json_text(json, "n", item.title, sizeof(item.title));
        if (s_lists == NULL)
            return;
        list_t *list = &s_lists[kind];
        taskENTER_CRITICAL(&s_lock);
        if (i == 0)
        {
            list->count = 0;
            list->open = true;
        }
        if (i >= 0 && i < DECK_LIST_MAX && i == list->count)
            list->items[list->count++] = item;
        if (i < 0)
        {
            if (!list->open)
                list->count = 0; /* only the end mark: the list is now empty */
            list->open = false;
            list->version++;
        }
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    if (strcmp(type, "host") == 0)
    {
        char name[24];
        deck_json_text(json, "name", name, sizeof(name));
        hid_link_name_host(name); /* shown in the host list of the watch */
        return;
    }
    if (strcmp(type, "agent") == 0)
    {
        taskENTER_CRITICAL(&s_lock);
        deck_json_text(json, "v", s_agent_version, sizeof(s_agent_version));
        taskEXIT_CRITICAL(&s_lock);
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    next = s_state;
    taskEXIT_CRITICAL(&s_lock);

    if (strcmp(type, "ctx") == 0)
    {
        next.app[0] = next.title[0] = '\0';
        deck_json_text(json, "app", next.app, sizeof(next.app));
        deck_json_text(json, "title", next.title, sizeof(next.title));
        /* Tell the agent which touchbar the watch chose (log and self-test). */
        char ack[64];
        snprintf(ack, sizeof(ack), "{\"t\":\"prof\",\"n\":\"%s\"}", deck_profile_name(next.app, next.title));
        hid_deck_send(ack);
    }
    else if (strcmp(type, "media") == 0)
    {
        next.track[0] = next.artist[0] = '\0';
        deck_json_text(json, "title", next.track, sizeof(next.track));
        deck_json_text(json, "artist", next.artist, sizeof(next.artist));
        next.playing = deck_json_number(json, "playing", 0) != 0;
    }
    else if (strcmp(type, "sys") == 0)
    {
        next.cpu = (int8_t)deck_json_number(json, "cpu", -1);
        next.ram = (int8_t)deck_json_number(json, "ram", -1);
        next.mic = (int8_t)deck_json_number(json, "mic", -1);
    }
    else if (strcmp(type, "res") == 0)
    {
        next.message[0] = '\0';
        deck_json_text(json, "msg", next.message, sizeof(next.message));
        if (deck_json_number(json, "beep", 0))
            next.alerts++;
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

void deck_core_init(void (*command)(const char *json))
{
    s_command = command;
    if (s_lists == NULL)
        s_lists = heap_caps_calloc(DECK_LIST_COUNT, sizeof(list_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    hid_deck_set_receiver(receive);
}

void deck_state_take(deck_state_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_state;
    s_state.message[0] = '\0'; /* a message is shown once */
    taskEXIT_CRITICAL(&s_lock);
}

bool deck_send_action(const char *action)
{
    static unsigned id; /* the agent runs each id once, whatever the radio repeats */
    char json[64];
    snprintf(json, sizeof(json), "{\"t\":\"act\",\"a\":\"%s\",\"id\":%u}", action, ++id);
    return hid_deck_send(json);
}

bool deck_send_key(const deck_key_t *key, bool mac)
{
    uint8_t mods = key->modifiers & ~DECK_CMD;
    if (key->modifiers & DECK_CMD)
        mods |= mac ? HIDL_MOD_GUI : HIDL_MOD_CTRL;
    return hid_link_key(mods, key->key);
}

/* ---- profiles ----
 *
 * One profile per application (and per site, matched on the window title).
 * Keys are HID shortcuts sent by the watch; a key with an `action` asks the
 * agent instead (lock, display off, microphone). Letters and function keys
 * only: punctuation moves between host keyboard layouts. */

#define C DECK_CMD
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
    K_KP_MINUS = 0x56, K_KP_PLUS = 0x57, K_KP_0 = 0x62,
};

/* deck_key_t: deck_core.h */

/* deck_profile_t: deck_core.h */

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
static const deck_key_t k_netflix[] = {
    {"Play / pausa", 0, K_SPACE}, {"Schermo intero", 0, K_F}, {"Muto", 0, K_M},       {"-10 s", 0, K_LEFT},
    {"+10 s", 0, K_RIGHT},        {"Volume +", 0, K_UP},      {"Volume -", 0, K_DOWN}, {"Salta intro", 0, K_S},
};
static const deck_key_t k_twitch[] = {
    {"Play / pausa", 0, K_SPACE}, {"Schermo intero", 0, K_F}, {"Muto", 0, K_M}, {"Modalita cinema", A, K_T},
};
static const deck_key_t k_meet[] = {
    {"Microfono", C, K_D},        {"Video", C, K_E},          {"Alza la mano", C | A, K_H}, {"Chat", C | A, K_C},
    {"Partecipanti", C | A, K_P}, {"Sottotitoli", 0, K_C},
};
static const deck_key_t k_photoshop[] = {
    {"Pennello", 0, K_B},    {"Gomma", 0, K_E},          {"Sposta", 0, K_V},            {"Selezione", 0, K_M},
    {"Lazo", 0, K_L},        {"Contagocce", 0, K_I},     {"Timbro", 0, K_S},            {"Testo", 0, K_T},
    {"Annulla", C, K_Z},     {"Ripeti", C | S, K_Z},     {"Nuovo livello", C | S, K_N}, {"Trasforma", C, K_T},
    {"Deseleziona", C, K_D}, {"Inverti selez.", C | S, K_I}, {"Adatta", C, K_0},        {"Salva", C, K_S},
};
static const deck_key_t k_illustrator[] = {
    {"Selezione", 0, K_V},  {"Selez. diretta", 0, K_A}, {"Penna", 0, K_P},     {"Testo", 0, K_T},
    {"Rettangolo", 0, K_M}, {"Ellisse", 0, K_L},        {"Pennello", 0, K_B},  {"Contagocce", 0, K_I},
    {"Raggruppa", C, K_G},  {"Separa", C | S, K_G},     {"Annulla", C, K_Z},   {"Ripeti", C | S, K_Z},
    {"Adatta", C, K_0},     {"Esporta", C | A, K_E},    {"Salva", C, K_S},     {"Righelli", C, K_R},
};
static const deck_key_t k_premiere[] = {
    {"Play / pausa", 0, K_SPACE}, {"Indietro J", 0, K_J}, {"Stop K", 0, K_K},    {"Avanti L", 0, K_L},
    {"Taglia qui", C, K_K},       {"Lama", 0, K_C},       {"Selezione", 0, K_V}, {"Elimina ripple", S, K_DEL},
    {"Marker", 0, K_M},           {"Punto In", 0, K_I},   {"Punto Out", 0, K_O}, {"Annulla", C, K_Z},
    {"Inizio", 0, K_HOME},        {"Fine", 0, K_END},     {"Esporta", C, K_M},   {"Salva", C, K_S},
};
static const deck_key_t k_aftereffects[] = {
    {"Anteprima", 0, K_SPACE},  {"Nuova comp.", C, K_N}, {"Nuovo solido", C, K_Y}, {"Dividi livello", C | S, K_D},
    {"Precomponi", C | S, K_C}, {"Annulla", C, K_Z},     {"Inizio", 0, K_HOME},    {"Fine", 0, K_END},
    {"Coda render", C, K_M},    {"Salva", C, K_S},       {"Selezione", 0, K_V},    {"Testo", C | A | S, K_T},
};
static const deck_key_t k_resolve[] = {
    {"Play / pausa", 0, K_SPACE}, {"Indietro J", 0, K_J}, {"Stop K", 0, K_K},      {"Avanti L", 0, K_L},
    {"Selezione", 0, K_A},        {"Lama", 0, K_B},       {"Dividi", C, K_B},      {"Marker", 0, K_M},
    {"Punto In", 0, K_I},         {"Punto Out", 0, K_O},  {"Annulla", C, K_Z},     {"Ripeti", C | S, K_Z},
    {"Inizio", 0, K_HOME},        {"Fine", 0, K_END},     {"Elimina ripple", S, K_DEL}, {"Salva", C, K_S},
};
static const deck_key_t k_blender[] = {
    {"Sposta", 0, K_G},          {"Ruota", 0, K_R},    {"Scala", 0, K_S},        {"Estrudi", 0, K_E},
    {"Modifica / oggetto", 0, K_TAB}, {"Elimina", 0, K_X}, {"Duplica", S, K_D},  {"Cerca", 0, K_F3},
    {"Seleziona tutto", 0, K_A}, {"Nascondi", 0, K_H}, {"Mostra tutto", A, K_H}, {"Vista camera", 0, K_KP_0},
    {"Render", 0, K_F12},        {"Annulla", C, K_Z},  {"Ripeti", C | S, K_Z},   {"Salva", C, K_S},
};
static const deck_key_t k_gimp[] = {
    {"Pennello", 0, K_P}, {"Gomma", S, K_E},             {"Sposta", 0, K_M},          {"Selezione", 0, K_R},
    {"Lazo", 0, K_F},     {"Testo", 0, K_T},             {"Contagocce", 0, K_O},      {"Annulla", C, K_Z},
    {"Ripeti", C, K_Y},   {"Nuovo livello", C | S, K_N}, {"Deseleziona", C | S, K_A}, {"Adatta", C | S, K_J},
    {"Esporta", C | S, K_E}, {"Salva", C, K_S},
};
static const deck_key_t k_figma[] = {
    {"Sposta", 0, K_V},     {"Cornice", 0, K_F},     {"Rettangolo", 0, K_R},     {"Ellisse", 0, K_O},
    {"Testo", 0, K_T},      {"Penna", 0, K_P},       {"Commento", 0, K_C},       {"Raggruppa", C, K_G},
    {"Separa", C | S, K_G}, {"Auto layout", S, K_A}, {"Componente", C | A, K_K}, {"Duplica", C, K_D},
    {"Adatta tutto", S, K_1}, {"Annulla", C, K_Z},   {"Esporta", C | S, K_E},    {"Nascondi", C | S, K_H},
};
static const deck_key_t k_outlook[] = {
    {"Nuova mail", C, K_N}, {"Rispondi", C, K_R},   {"Rispondi a tutti", C | S, K_R}, {"Inoltra", C, K_F},
    {"Invia", A, K_S},      {"Elimina", 0, K_DEL},  {"Letto", C, K_Q},                {"Non letto", C, K_U},
    {"Posta", C, K_1},      {"Calendario", C, K_2}, {"Contatti", C, K_3},             {"Cerca", C, K_E},
};
static const deck_key_t k_slack[] = {
    {"Vai a", C, K_K},         {"Non letti", C | S, K_A},   {"Menzioni", C | S, K_M}, {"Messaggi diretti", C | S, K_K},
    {"Canale prec.", A, K_UP}, {"Canale succ.", A, K_DOWN}, {"Huddle", C | S, K_H},   {"Cerca", C, K_G},
};
static const deck_key_t k_notion[] = {
    {"Cerca", C, K_P},  {"Nuova pagina", C, K_N}, {"Grassetto", C, K_B},    {"Corsivo", C, K_I},
    {"Codice", C, K_E}, {"Link", C, K_K},         {"Commento", C | S, K_M}, {"Annulla", C, K_Z},
};
static const deck_key_t k_obsidian[] = {
    {"Comandi", C, K_P},         {"Apri nota", C, K_O},         {"Nuova nota", C, K_N},  {"Cerca", C | S, K_F},
    {"Grafo", C, K_G},           {"Modifica / vista", C, K_E},  {"Grassetto", C, K_B},   {"Corsivo", C, K_I},
    {"Indietro", C | A, K_LEFT}, {"Avanti", C | A, K_RIGHT},
};
static const deck_key_t k_jetbrains[] = {
    {"Azioni", C | S, K_A},  {"Apri file", C | S, K_N}, {"Esegui", S, K_F10},    {"Debug", S, K_F9},
    {"Breakpoint", C, K_F8}, {"Step over", 0, K_F8},    {"Step into", 0, K_F7},  {"Riformatta", C | A, K_L},
    {"Rinomina", S, K_F6},   {"Definizione", C, K_B},   {"Usi", A, K_F7},        {"Terminale", A, K_F12},
    {"Progetto", A, K_1},    {"Commit", C, K_K},        {"Aggiorna", C, K_T},    {"Salva", C, K_S},
};
static const deck_key_t k_visualstudio[] = {
    {"Compila", C | S, K_B}, {"Avvia", 0, K_F5},      {"Senza debug", C, K_F5},       {"Ferma", S, K_F5},
    {"Breakpoint", 0, K_F9}, {"Step over", 0, K_F10}, {"Step into", 0, K_F11},        {"Definizione", 0, K_F12},
    {"Trova", C, K_F},       {"Vai a tutto", C, K_T}, {"Soluzione", C | A, K_L},      {"Salva tutto", C | S, K_S},
};
static const deck_key_t k_notepadpp[] = {
    {"Nuovo", C, K_N},    {"Apri", C, K_O},          {"Salva", C, K_S},      {"Chiudi", C, K_W},
    {"Trova", C, K_F},    {"Sostituisci", C, K_H},   {"Vai a riga", C, K_G}, {"Duplica riga", C, K_D},
    {"Commenta", C, K_Q}, {"Maiuscolo", C | S, K_U}, {"Minuscolo", C, K_U},  {"Annulla", C, K_Z},
};
static const deck_key_t k_vlc[] = {
    {"Play / pausa", 0, K_SPACE}, {"Schermo intero", 0, K_F}, {"Muto", 0, K_M},       {"Stop", 0, K_S},
    {"-10 s", A, K_LEFT},         {"+10 s", A, K_RIGHT},      {"Volume +", C, K_UP},   {"Volume -", C, K_DOWN},
    {"Precedente", 0, K_P},       {"Successivo", 0, K_N},     {"Sottotitoli", 0, K_V}, {"Traccia audio", 0, K_B},
};
static const deck_key_t k_acrobat[] = {
    {"Pagina giu", 0, K_PGDN}, {"Pagina su", 0, K_PGUP},   {"Prima", 0, K_HOME},       {"Ultima", 0, K_END},
    {"Cerca", C, K_F},         {"Zoom +", C, K_KP_PLUS},   {"Zoom -", C, K_KP_MINUS},  {"Adatta pagina", C, K_0},
    {"Adatta larghezza", C, K_2}, {"Lettura", C, K_H},     {"Schermo intero", C, K_L}, {"Stampa", C, K_P},
};
static const deck_key_t k_generic[] = {
    {"Copia", C, K_C},         {"Incolla", C, K_V},       {"Taglia", C, K_X},          {"Annulla", C, K_Z},
    {"Ripeti", C, K_Y},        {"Seleziona tutto", C, K_A}, {"Cambia app", A, K_TAB},  {"Chiudi finestra", A, K_F4},
};
static const deck_key_t k_system[] = {
    {"Cambia app", 0, 0, "@switch"},       {"App aperte", 0, 0, "@windows"},     {"Vista attivita", W, K_TAB},
    {"Blocca PC", 0, 0, "lock"},           {"Schermo off", 0, 0, "display_off"}, {"Microfono", 0, 0, "mic"},
    {"Task manager", C | S, K_ESC},        {"Screenshot", W | S, K_S},           {"Desktop", W, K_D},
    {"Esplora file", W, K_E},              {"Impostazioni", W, K_I},             {"Appunti", W, K_V},
    {"Cerca", W, K_S},                     {"Esegui", W, K_R},                   {"Notifiche", W, K_N},
    {"Azioni rapide", W, K_A},             {"Dettatura", W, K_H},                {"Proietta", W, K_P},
    {"Affianca sx", W, K_LEFT},            {"Affianca dx", W, K_RIGHT},          {"Massimizza", W, K_UP},
    {"Riduci", W, K_DOWN},                 {"Chiudi finestra", A, K_F4},         {"Game bar", W, K_G},
    {"Registra clip", W | A, K_G},         {"Desktop succ.", W | HIDL_MOD_CTRL, K_RIGHT},
    {"Desktop prec.", W | HIDL_MOD_CTRL, K_LEFT},
    {"Nuovo desktop", W | HIDL_MOD_CTRL, K_D}, {"Chiudi desktop", W | HIDL_MOD_CTRL, K_F4},
    {"Sospendi PC", 0, 0, "sleep"},
};

#define BROWSERS "chrome,msedge,firefox,brave,opera,vivaldi,chromium"

/* First match wins: site profiles come before their browser. */
static const deck_profile_t s_profiles[] = {
    {BROWSERS, "YouTube", "YouTube", LV_SYMBOL_VIDEO, UI_COLOR_RED, KEYS(k_youtube)},
    {BROWSERS, "Netflix", "Netflix", LV_SYMBOL_VIDEO, UI_COLOR_RED, KEYS(k_netflix)},
    {BROWSERS, "Twitch", "Twitch", LV_SYMBOL_VIDEO, UI_COLOR_PURPLE, KEYS(k_twitch)},
    {BROWSERS, "Meet", "Google Meet", LV_SYMBOL_CALL, UI_COLOR_GREEN, KEYS(k_meet)},
    {BROWSERS, "Google Docs", "Google Docs", LV_SYMBOL_FILE, UI_COLOR_BLUE, KEYS(k_word)},
    {BROWSERS, "Documenti Google", "Google Docs", LV_SYMBOL_FILE, UI_COLOR_BLUE, KEYS(k_word)},
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
    {"photoshop", NULL, "Photoshop", LV_SYMBOL_IMAGE, UI_COLOR_BLUE, KEYS(k_photoshop)},
    {"illustrator", NULL, "Illustrator", LV_SYMBOL_EDIT, UI_COLOR_ORANGE, KEYS(k_illustrator)},
    {"adobe premiere pro", NULL, "Premiere Pro", LV_SYMBOL_VIDEO, UI_COLOR_PURPLE, KEYS(k_premiere)},
    {"afterfx", NULL, "After Effects", LV_SYMBOL_VIDEO, UI_COLOR_INDIGO, KEYS(k_aftereffects)},
    {"resolve", NULL, "DaVinci Resolve", LV_SYMBOL_VIDEO, UI_COLOR_ORANGE, KEYS(k_resolve)},
    {"blender", NULL, "Blender", LV_SYMBOL_IMAGE, UI_COLOR_ORANGE, KEYS(k_blender)},
    {"gimp,gimp-2.10,gimp-3.0", NULL, "GIMP", LV_SYMBOL_IMAGE, UI_COLOR_GRAY, KEYS(k_gimp)},
    {"figma", NULL, "Figma", LV_SYMBOL_EDIT, UI_COLOR_PINK, KEYS(k_figma)},
    {"outlook,olk", NULL, "Outlook", LV_SYMBOL_ENVELOPE, UI_COLOR_BLUE, KEYS(k_outlook)},
    {"slack", NULL, "Slack", LV_SYMBOL_CALL, UI_COLOR_PURPLE, KEYS(k_slack)},
    {"notion", NULL, "Notion", LV_SYMBOL_FILE, UI_COLOR_GRAY, KEYS(k_notion)},
    {"obsidian", NULL, "Obsidian", LV_SYMBOL_FILE, UI_COLOR_PURPLE, KEYS(k_obsidian)},
    {"idea64,pycharm64,webstorm64,clion64,rider64,goland64,phpstorm64,studio64", NULL, "JetBrains",
     LV_SYMBOL_EDIT, UI_COLOR_ORANGE, KEYS(k_jetbrains)},
    {"devenv", NULL, "Visual Studio", LV_SYMBOL_EDIT, UI_COLOR_PURPLE, KEYS(k_visualstudio)},
    {"notepad++", NULL, "Notepad++", LV_SYMBOL_EDIT, UI_COLOR_GREEN, KEYS(k_notepadpp)},
    {"vlc", NULL, "VLC", LV_SYMBOL_VIDEO, UI_COLOR_ORANGE, KEYS(k_vlc)},
    {"acrobat,acrord32", NULL, "Acrobat", LV_SYMBOL_FILE, UI_COLOR_RED, KEYS(k_acrobat)},
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

const deck_profile_t *deck_profile_for(const char *app, const char *title)
{
    for (size_t i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++)
    {
        const deck_profile_t *p = &s_profiles[i];
        if ((p->apps == NULL || in_list(p->apps, app)) && (p->title == NULL || strstr(title, p->title) != NULL))
            return p;
    }
    return &s_generic;
}

const char *deck_profile_name(const char *app, const char *title)
{
    const deck_profile_t *p = deck_profile_for(app, title);
    return p->name[0] ? p->name : "Generale";
}

const deck_key_t *deck_system_keys(int *count)
{
    *count = (int)(sizeof(k_system) / sizeof(k_system[0]));
    return k_system;
}

int deck_list(deck_list_t kind, deck_item_t *out, int max, uint32_t *version)
{
    *version = 0;
    if (s_lists == NULL || kind >= DECK_LIST_COUNT)
        return 0;
    list_t *list = &s_lists[kind];
    taskENTER_CRITICAL(&s_lock);
    int count = list->count < max ? list->count : max;
    memcpy(out, list->items, (size_t)count * sizeof(*out));
    *version = list->version;
    taskEXIT_CRITICAL(&s_lock);
    return count;
}

/* "Cambia app": Alt (Cmd) stays down while taps follow each other, as on a
 * keyboard; it lets go once the taps stop and the chosen window comes forward. */
#define SWITCH_HOLD_US (1200 * 1000)

static esp_timer_handle_t s_switch_timer;

static void switch_release(void *arg)
{
    (void)arg;
    hid_link_keyboard(0, 0);
}

static bool switch_app(bool mac)
{
    if (s_switch_timer == NULL)
    {
        const esp_timer_create_args_t args = {.callback = switch_release, .name = "deck_switch"};
        if (esp_timer_create(&args, &s_switch_timer) != ESP_OK)
            return false;
    }
    uint8_t hold = mac ? HIDL_MOD_GUI : HIDL_MOD_ALT;
    bool ok = hid_link_keyboard(hold, K_TAB) && hid_link_keyboard(hold, 0);
    esp_timer_stop(s_switch_timer);
    esp_timer_start_once(s_switch_timer, SWITCH_HOLD_US);
    return ok;
}

bool deck_run_key(const deck_key_t *key, bool mac)
{
    if (key->action == NULL)
        return deck_send_key(key, mac);
    if (strcmp(key->action, "@switch") == 0)
        return switch_app(mac);
    if (strcmp(key->action, "@windows") == 0) /* task view / Mission Control */
        return mac ? hid_link_key(HIDL_MOD_CTRL, K_UP) : hid_link_key(HIDL_MOD_GUI, K_TAB);
    return deck_send_action(key->action);
}
