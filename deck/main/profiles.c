/*
 * Deck profiles (see deck.h). Built-in profiles are code; custom profiles live
 * in <storage>/config/deck.json, written by the editor page and validated here
 * (every action is parsed before anything is saved).
 */
#include "deck.h"
#include "action.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "nvs.h"
#include "services/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "deck";

#define K(label, glyph, color, action) {label, glyph, color, action}

static const deck_profile_t s_builtin[] = {
    {"Riunioni", DECK_DIAL_VOLUME, false, 12, {
        K("Mic Teams", "MIC", 0xFF6B6B, "ctrl+shift+m"),
        K("Cam Teams", "video", 0x4FB3FF, "ctrl+shift+o"),
        K("Mano", "up", 0xF7C948, "ctrl+shift+k"),
        K("Esci Teams", "call", 0xFF453A, "ctrl+shift+h"),
        K("Mic Meet", "MIC", 0xFF8A80, "ctrl+d"),
        K("Cam Meet", "video", 0x80C8FF, "ctrl+e"),
        K("Mic Zoom", "MIC", 0xFFA07A, "alt+a"),
        K("Cam Zoom", "video", 0x9AD0FF, "alt+v"),
        K("Condividi", "image", 0x9C8CFF, "ctrl+shift+e"),
        K("PC muto", "mute", 0xB0B0C0, "media:mute"),
        K("Screenshot", "cut", 0x5BD6A0, "win+shift+s"),
        K("Blocca PC", "power", 0xFF9F0A, "win+l"),
    }},
    {"Windows", DECK_DIAL_SCROLL, false, 12, {
        K("Desktop", "home", 0x4FB3FF, "win+d"),
        K("Attività", "list", 0x9C8CFF, "win+tab"),
        K("Esplora file", "folder", 0xF7C948, "win+e"),
        K("Appunti", "paste", 0x5BD6A0, "win+v"),
        K("Screenshot", "cut", 0x5BD6A0, "win+shift+s"),
        K("Impostazioni", "settings", 0xB0B0C0, "win+i"),
        K("Desktop prec.", "left", 0x80C8FF, "ctrl+win+left"),
        K("Desktop succ.", "right", 0x80C8FF, "ctrl+win+right"),
        K("Terminale", ">_", 0x30D158, "win+r; wait:350; type:wt; enter"),
        K("Task manager", "bars", 0xFF9F0A, "ctrl+shift+esc"),
        K("Emoji", ":)", 0xFFD60A, "win+period"),
        K("Blocca PC", "power", 0xFF453A, "win+l"),
    }},
    {"Codice", DECK_DIAL_UNDO, false, 12, {
        K("Comandi", "keyboard", 0x4FB3FF, "ctrl+shift+p"),
        K("Apri file", "file", 0x80C8FF, "ctrl+p"),
        K("Pannello", "bars", 0x9C8CFF, "ctrl+j"),
        K("Commenta", "//", 0x5BD6A0, "ctrl+k ctrl+c"),
        K("Togli //", "/-", 0x5BD6A0, "ctrl+k ctrl+u"),
        K("Formatta", "edit", 0xF7C948, "shift+alt+f"),
        K("Rinomina", "F2", 0xFFA07A, "f2"),
        K("Definizione", "F12", 0xFFA07A, "f12"),
        K("Debug", "play", 0x30D158, "f5"),
        K("Stop", "stop", 0xFF453A, "shift+f5"),
        K("Passo", "next", 0xF7C948, "f10"),
        K("Git", "loop", 0xFF9F0A, "ctrl+shift+g"),
    }},
    {"Browser", DECK_DIAL_TABS, false, 12, {
        K("Nuova scheda", "plus", 0x4FB3FF, "ctrl+t"),
        K("Riapri", "refresh", 0x80C8FF, "ctrl+shift+t"),
        K("Chiudi", "close", 0xFF6B6B, "ctrl+w"),
        K("Indietro", "left", 0xB0B0C0, "alt+left"),
        K("Avanti", "right", 0xB0B0C0, "alt+right"),
        K("Ricarica", "loop", 0x5BD6A0, "f5"),
        K("Indirizzo", "edit", 0xF7C948, "ctrl+l"),
        K("Incognito", "eye_closed", 0x9C8CFF, "ctrl+shift+n"),
        K("Cronologia", "list", 0x9C8CFF, "ctrl+h"),
        K("Download", "download", 0x30D158, "ctrl+j"),
        K("Zoom 100%", "1:1", 0xFFA07A, "ctrl+0"),
        K("Strumenti", "F12", 0xFF9F0A, "f12"),
    }},
    {"Media", DECK_DIAL_VOLUME, false, 12, {
        K("Play", "play", 0x30D158, "media:play"),
        K("Precedente", "prev", 0x4FB3FF, "media:prev"),
        K("Successivo", "next", 0x4FB3FF, "media:next"),
        K("Muto", "mute", 0xFF6B6B, "media:mute"),
        K("Volume -", "voldown", 0xB0B0C0, "media:voldown"),
        K("Volume +", "volup", 0xB0B0C0, "media:volup"),
        K("YouTube", "-10", 0xFF453A, "j"),
        K("YouTube", "+10", 0xFF453A, "l"),
        K("Intero", "image", 0x9C8CFF, "f"),
        K("Sottotitoli", "CC", 0xF7C948, "c"),
        K("Stop", "stop", 0xFF9F0A, "media:stop"),
        K("Calcola", "=", 0x5BD6A0, "media:calc"),
    }},
    {"Presentazioni", DECK_DIAL_ARROWS, false, 8, {
        K("Avanti", "right", 0x30D158, "right"),
        K("Indietro", "left", 0xB0B0C0, "left"),
        K("Dall'inizio", "play", 0x4FB3FF, "f5"),
        K("Da qui", "next", 0x80C8FF, "shift+f5"),
        K("Nero", "eye_closed", 0x9C8CFF, "b"),
        K("Laser", "gps", 0xFF453A, "ctrl+l"),
        K("Fine", "stop", 0xFF6B6B, "esc"),
        K("Penna", "edit", 0xF7C948, "ctrl+p"),
    }},
};

#define BUILTIN (int)(sizeof(s_builtin) / sizeof(s_builtin[0]))

static deck_profile_t *s_custom; /* PSRAM, DECK_CUSTOM */
static int s_custom_count;

/* ------------------------------------------------------------ names */

static const char *const s_dial_ids[DECK_DIAL_COUNT] = {"volume", "scroll", "zoom", "tabs", "undo", "arrows", "arrows_v"};
static const char *const s_dial_names[DECK_DIAL_COUNT] = {"Volume", "Scorri", "Zoom", "Schede", "Annulla / ripeti",
                                                          "Frecce", "Frecce su/giù"};

const char *deck_dial_name(deck_dial_t dial) { return dial < DECK_DIAL_COUNT ? s_dial_names[dial] : ""; }
const char *deck_dial_id(deck_dial_t dial) { return dial < DECK_DIAL_COUNT ? s_dial_ids[dial] : "volume"; }

typedef struct
{
    const char *name, *symbol;
} glyph_t;

static const glyph_t s_glyphs[] = {
    {"mic", LV_SYMBOL_AUDIO},       {"video", LV_SYMBOL_VIDEO},       {"mute", LV_SYMBOL_MUTE},
    {"volup", LV_SYMBOL_VOLUME_MAX}, {"voldown", LV_SYMBOL_VOLUME_MID}, {"play", LV_SYMBOL_PLAY},
    {"pause", LV_SYMBOL_PAUSE},     {"stop", LV_SYMBOL_STOP},         {"next", LV_SYMBOL_NEXT},
    {"prev", LV_SYMBOL_PREV},       {"up", LV_SYMBOL_UP},             {"down", LV_SYMBOL_DOWN},
    {"left", LV_SYMBOL_LEFT},       {"right", LV_SYMBOL_RIGHT},       {"plus", LV_SYMBOL_PLUS},
    {"minus", LV_SYMBOL_MINUS},     {"close", LV_SYMBOL_CLOSE},       {"ok", LV_SYMBOL_OK},
    {"home", LV_SYMBOL_HOME},       {"list", LV_SYMBOL_LIST},         {"settings", LV_SYMBOL_SETTINGS},
    {"power", LV_SYMBOL_POWER},     {"image", LV_SYMBOL_IMAGE},       {"refresh", LV_SYMBOL_REFRESH},
    {"loop", LV_SYMBOL_LOOP},       {"shuffle", LV_SYMBOL_SHUFFLE},   {"edit", LV_SYMBOL_EDIT},
    {"copy", LV_SYMBOL_COPY},       {"cut", LV_SYMBOL_CUT},           {"paste", LV_SYMBOL_PASTE},
    {"save", LV_SYMBOL_SAVE},       {"file", LV_SYMBOL_FILE},         {"folder", LV_SYMBOL_DIRECTORY},
    {"download", LV_SYMBOL_DOWNLOAD}, {"upload", LV_SYMBOL_UPLOAD},   {"trash", LV_SYMBOL_TRASH},
    {"keyboard", LV_SYMBOL_KEYBOARD}, {"bell", LV_SYMBOL_BELL},       {"envelope", LV_SYMBOL_ENVELOPE},
    {"call", LV_SYMBOL_CALL},       {"wifi", LV_SYMBOL_WIFI},         {"bluetooth", LV_SYMBOL_BLUETOOTH},
    {"usb", LV_SYMBOL_USB},         {"warning", LV_SYMBOL_WARNING},   {"eye", LV_SYMBOL_EYE_OPEN},
    {"eye_closed", LV_SYMBOL_EYE_CLOSE}, {"gps", LV_SYMBOL_GPS},      {"charge", LV_SYMBOL_CHARGE},
    {"bars", LV_SYMBOL_BARS},       {"eject", LV_SYMBOL_EJECT},       {"drive", LV_SYMBOL_DRIVE},
};

const char *deck_glyph(const char *name)
{
    for (unsigned i = 0; i < sizeof(s_glyphs) / sizeof(s_glyphs[0]); i++)
        if (strcmp(s_glyphs[i].name, name) == 0)
            return s_glyphs[i].symbol;
    return name;
}

/* ------------------------------------------------------------ access */

int deck_profile_count(void) { return BUILTIN + s_custom_count; }

const deck_profile_t *deck_profile(int index)
{
    if (index < BUILTIN)
        return &s_builtin[index < 0 ? 0 : index];
    index -= BUILTIN;
    return index < s_custom_count ? &s_custom[index] : &s_builtin[0];
}

/* ------------------------------------------------------------ JSON */

static int dial_from_id(const char *id)
{
    for (int i = 0; i < DECK_DIAL_COUNT; i++)
        if (id && strcmp(id, s_dial_ids[i]) == 0)
            return i;
    return -1;
}

static void copy_text(char *out, size_t size, const cJSON *item)
{
    snprintf(out, size, "%s", cJSON_IsString(item) ? item->valuestring : "");
}

/* Parse + validate; fills out[] (count) or explains the first problem. */
static bool parse_custom(const char *json, deck_profile_t *out, int *count, char *error, size_t error_size)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *list = cJSON_GetObjectItem(root, "profiles");
    *count = 0;
    if (!cJSON_IsArray(list))
    {
        snprintf(error, error_size, "JSON non valido");
        cJSON_Delete(root);
        return false;
    }
    const cJSON *p;
    cJSON_ArrayForEach(p, list)
    {
        if (*count == DECK_CUSTOM)
        {
            snprintf(error, error_size, "Al massimo %d profili personali", DECK_CUSTOM);
            cJSON_Delete(root);
            return false;
        }
        deck_profile_t *profile = &out[*count];
        memset(profile, 0, sizeof(*profile));
        profile->custom = true;
        copy_text(profile->name, sizeof(profile->name), cJSON_GetObjectItem(p, "name"));
        int dial = dial_from_id(cJSON_IsString(cJSON_GetObjectItem(p, "dial")) ? cJSON_GetObjectItem(p, "dial")->valuestring : "");
        profile->dial = (uint8_t)(dial < 0 ? DECK_DIAL_VOLUME : dial);
        if (!profile->name[0])
        {
            snprintf(error, error_size, "Profilo %d: manca il nome", *count + 1);
            cJSON_Delete(root);
            return false;
        }
        const cJSON *keys = cJSON_GetObjectItem(p, "keys"), *k;
        cJSON_ArrayForEach(k, keys)
        {
            if (profile->count == DECK_KEYS)
                break;
            deck_key_t *key = &profile->keys[profile->count];
            copy_text(key->label, sizeof(key->label), cJSON_GetObjectItem(k, "label"));
            copy_text(key->glyph, sizeof(key->glyph), cJSON_GetObjectItem(k, "glyph"));
            copy_text(key->action, sizeof(key->action), cJSON_GetObjectItem(k, "action"));
            const cJSON *color = cJSON_GetObjectItem(k, "color");
            key->color = cJSON_IsString(color) ? (uint32_t)strtoul(color->valuestring + (color->valuestring[0] == '#'), NULL, 16)
                                               : 0x4FB3FF;
            if (!key->label[0] && !key->action[0])
                continue; /* empty slot */
            deck_action_t parsed;
            if (!deck_action_parse(key->action, &parsed))
            {
                snprintf(error, error_size, "%s, tasto \"%s\": %s (carattere %d)", profile->name, key->label,
                         parsed.error, parsed.error_at + 1);
                cJSON_Delete(root);
                return false;
            }
            profile->count++;
        }
        (*count)++;
    }
    cJSON_Delete(root);
    return true;
}

static void *psram_malloc(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

esp_err_t deck_profiles_load(void)
{
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    if (s_custom == NULL)
        s_custom = heap_caps_calloc(DECK_CUSTOM, sizeof(deck_profile_t), MALLOC_CAP_SPIRAM);
    if (s_custom == NULL)
        return ESP_ERR_NO_MEM;
    s_custom_count = 0;
    char path[96];
    if (!storage_config_path("deck.json", path, sizeof(path)))
        return ESP_ERR_NOT_FOUND;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return ESP_ERR_NOT_FOUND;
    char *json = heap_caps_calloc(1, 32 * 1024, MALLOC_CAP_SPIRAM);
    size_t length = json ? fread(json, 1, 32 * 1024 - 1, f) : 0;
    fclose(f);
    char error[96];
    int count = 0;
    bool ok = json && length && parse_custom(json, s_custom, &count, error, sizeof(error));
    free(json);
    if (!ok)
    {
        ESP_LOGW(TAG, "Custom profiles ignored: %s", json ? error : "no memory");
        return ESP_ERR_INVALID_ARG;
    }
    s_custom_count = count;
    ESP_LOGI(TAG, "%d custom profiles", count);
    return ESP_OK;
}

esp_err_t deck_profiles_store(const char *json, char *error, size_t error_size)
{
    deck_profile_t *parsed = heap_caps_calloc(DECK_CUSTOM, sizeof(deck_profile_t), MALLOC_CAP_SPIRAM);
    int count = 0;
    if (parsed == NULL)
    {
        snprintf(error, error_size, "Memoria insufficiente");
        return ESP_ERR_NO_MEM;
    }
    if (!parse_custom(json, parsed, &count, error, error_size))
    {
        free(parsed);
        return ESP_ERR_INVALID_ARG;
    }
    char path[96], temp[100];
    if (!storage_config_path("deck.json", path, sizeof(path)))
    {
        snprintf(error, error_size, "Nessuna memoria per i file (microSD o flash)");
        free(parsed);
        return ESP_ERR_NOT_FOUND;
    }
    /* Write a temporary file first: a power cut never leaves half a configuration. */
    snprintf(temp, sizeof(temp), "%s.new", path);
    FILE *f = fopen(temp, "wb");
    bool ok = f && fwrite(json, 1, strlen(json), f) == strlen(json);
    if (f)
        fclose(f);
    remove(path);
    ok = ok && rename(temp, path) == 0;
    if (ok)
    {
        memcpy(s_custom, parsed, sizeof(deck_profile_t) * DECK_CUSTOM);
        s_custom_count = count;
    }
    else
        snprintf(error, error_size, "Scrittura non riuscita");
    free(parsed);
    return ok ? ESP_OK : ESP_FAIL;
}

char *deck_profiles_json(void)
{
    cJSON *root = cJSON_CreateObject(), *list = cJSON_AddArrayToObject(root, "profiles");
    for (int i = 0; i < deck_profile_count(); i++)
    {
        const deck_profile_t *p = deck_profile(i);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", p->name);
        cJSON_AddStringToObject(item, "dial", deck_dial_id((deck_dial_t)p->dial));
        cJSON_AddBoolToObject(item, "custom", p->custom);
        cJSON *keys = cJSON_AddArrayToObject(item, "keys");
        for (int k = 0; k < p->count; k++)
        {
            cJSON *key = cJSON_CreateObject();
            char color[8];
            snprintf(color, sizeof(color), "#%06lX", (unsigned long)(p->keys[k].color & 0xFFFFFF));
            cJSON_AddStringToObject(key, "label", p->keys[k].label);
            cJSON_AddStringToObject(key, "glyph", p->keys[k].glyph);
            cJSON_AddStringToObject(key, "color", color);
            cJSON_AddStringToObject(key, "action", p->keys[k].action);
            cJSON_AddItemToArray(keys, key);
        }
        cJSON_AddItemToArray(list, item);
    }
    cJSON *dials = cJSON_AddArrayToObject(root, "dials");
    for (int d = 0; d < DECK_DIAL_COUNT; d++)
    {
        cJSON *dial = cJSON_CreateObject();
        cJSON_AddStringToObject(dial, "id", s_dial_ids[d]);
        cJSON_AddStringToObject(dial, "name", s_dial_names[d]);
        cJSON_AddItemToArray(dials, dial);
    }
    cJSON *glyphs = cJSON_AddArrayToObject(root, "glyphs");
    for (unsigned g = 0; g < sizeof(s_glyphs) / sizeof(s_glyphs[0]); g++)
        cJSON_AddItemToArray(glyphs, cJSON_CreateString(s_glyphs[g].name));
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

/* ------------------------------------------------------------ settings */

static deck_settings_t s_settings = {.transport = 1 /* HID_LINK_BLE */};

const deck_settings_t *deck_settings(void)
{
    static bool loaded;
    if (!loaded)
    {
        loaded = true;
        nvs_handle_t nvs;
        if (nvs_open("deck", NVS_READONLY, &nvs) == ESP_OK)
        {
            deck_settings_t saved;
            size_t size = sizeof(saved);
            if (nvs_get_blob(nvs, "settings", &saved, &size) == ESP_OK && size == sizeof(saved))
                s_settings = saved;
            nvs_close(nvs);
        }
    }
    return &s_settings;
}

void deck_settings_save(const deck_settings_t *settings)
{
    s_settings = *settings;
    nvs_handle_t nvs;
    if (nvs_open("deck", NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_blob(nvs, "settings", &s_settings, sizeof(s_settings));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}
