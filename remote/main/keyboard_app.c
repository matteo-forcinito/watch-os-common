/*
 * Remote Control keyboard: big keys over the whole screen + swipe typing.
 *
 *   ┌──────────────────────────────┐  0
 *   │ <   ...testo scritto|      ● │      preview of what was sent + link state
 *   ├─────────┬─────────┬──────────┤  52
 *   │  ciao   │  cosa   │  casa    │      swipe suggestions (tap = replace)
 *   ├──┬──┬──┬──┬──┬──┬──┬─────────┤  96
 *   │q │w │e │r │t │y │u │             4 rows x 7 big keys (~52 x 70 px)
 *   │i │o │p │a │s │d │f │
 *   │g │h │j │k │l │z │x │
 *   │c │v │b │n │m │, │. │
 *   ├──┴──┴──┴──┴──┴──┴──┴─────────┤  376
 *   │ ⇧ │123│ ' │  spazio  │⌫ │⏎ │      controls
 *   └──────────────────────────────┘  448
 *
 * Touch rules on the letter area:
 *   tap            the key where the finger went DOWN (a small slip while
 *                  lifting never changes the letter); a bubble shows it
 *   hold a vowel   ~0.45 s: the accented vowel (e -> è, a -> à ...)
 *   swipe          slide over the letters of a word without lifting: the
 *                  dictionary finds the word, adds the space before it and
 *                  shows two alternatives; tap one to swap it
 *   ⌫ after swipe  deletes the whole swiped word
 *   space space    ". " (end of sentence), capital letter after it
 *
 * The keys are drawn by plain LVGL objects but the touch is handled by one
 * "pad" object, so a swipe is never broken into separate key presses.
 */

#include "remote.h"
#include <stdio.h>

#include "core/app.h"
#include "core/lv_port.h"
#include "hardware/board.h"
#include "ui/ui.h"
#include "ui/keyboard_layout.h"

#include "esp_log.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "keyboard";

#define COLS 7
#define ROWS 4
#define KEYS (COLS * ROWS)
#define PREVIEW_H 36
#define SUGGEST_H 32
#define CONTROL_H 44
#define TRAIL_MAX 48
#define TAIL_MAX 256
#define SUGGESTIONS 3
#define HOLD_MS 450
#define DOUBLE_SPACE_MS 900

typedef enum
{
    LAYER_LETTERS,
    LAYER_NUMBERS,
    LAYER_SYMBOLS,
} layer_t;

typedef enum
{
    SHIFT_OFF,
    SHIFT_ONCE,
    SHIFT_LOCK,
} shift_t;

typedef enum
{
    CASE_LOWER,
    CASE_FIRST,
    CASE_ALL,
} word_case_t;

/* One key: the text it types, or a key code for the special keys. */
typedef struct
{
    const char *text;
    const char *label; /* NULL = same as text */
    uint8_t keycode;   /* != 0: a key, not text */
} key_def_t;

#define K(t) {t, NULL, 0}
#define KC(l, code) {NULL, l, code}

static const key_def_t s_layers[3][KEYS] = {
    [LAYER_LETTERS] =
        {
            K("q"), K("w"), K("e"), K("r"), K("t"), K("y"), K("u"), K("i"), K("o"), K("p"),
            K("a"), K("s"), K("d"), K("f"), K("g"), K("h"), K("j"), K("k"), K("l"), K("z"),
            K("x"), K("c"), K("v"), K("b"), K("n"), K("m"), K(","), K("."),
        },
    [LAYER_NUMBERS] =
        {
            K("1"), K("2"), K("3"), K("4"), K("5"), K("6"), K("7"), K("8"), K("9"), K("0"),
            K("è"), K("é"), K("à"), K("ò"), K("ì"), K("ù"), K("?"), K("!"), K("@"), K("#"),
            K("€"), K("("), K(")"), K("-"), K("+"), K("/"), K(":"), K(";"),
        },
    [LAYER_SYMBOLS] =
        {
            K("*"),
            K("&"),
            K("%"),
            K("="),
            K("\""),
            K("_"),
            K("~"),
            K("<"),
            K(">"),
            K("["),
            K("]"),
            K("{"),
            K("}"),
            K("|"),
            K("\\"),
            K("^"),
            K("`"),
            K("$"),
            K("£"),
            K("°"),
            K("§"),
            KC("Esc", HIDL_KEY_ESC),
            KC("Tab", HIDL_KEY_TAB),
            KC(LV_SYMBOL_LEFT, HIDL_KEY_LEFT),
            KC(LV_SYMBOL_DOWN, HIDL_KEY_DOWN),
            KC(LV_SYMBOL_UP, HIDL_KEY_UP),
            KC(LV_SYMBOL_RIGHT, HIDL_KEY_RIGHT),
            KC("Canc", HIDL_KEY_DELETE),
        },
};

/* Hold a vowel for its accented form (Italian). */
static const char *accent_of(const char *text)
{
    static const char *const map[][2] = {
        {"a", "à"}, {"e", "è"}, {"i", "ì"}, {"o", "ò"}, {"u", "ù"},
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    {
        if (strcmp(text, map[i][0]) == 0)
        {
            return map[i][1];
        }
    }

    return NULL;
}

typedef struct
{
    lv_obj_t *screen;
    lv_obj_t *preview;
    lv_obj_t *dot;
    lv_obj_t *suggest[SUGGESTIONS];
    lv_obj_t *suggest_label[SUGGESTIONS];
    lv_obj_t *pad;
    lv_obj_t *caps[KEYS];
    lv_obj_t *cap_labels[KEYS];
    lv_obj_t *trail;
    lv_obj_t *bubble;
    lv_obj_t *bubble_label;
    lv_obj_t *shift_label;
    lv_obj_t *layer_label;
    lv_timer_t *status_timer;

    layer_t layer;
    shift_t shift;
    int32_t pad_w;
    int32_t pad_h;
    float key_w;
    float key_h;
    swipe_layout_t layout;
    ui_keyboard_layout_t geometry;

    /* current touch */
    bool touching;
    bool swiping;
    int down_key;
    int last_key;
    int distinct_keys;
    float path_length;
    uint32_t down_tick;
    swipe_point_t path[SWIPE_MAX_POINTS];
    int path_count;
    float path_step;
    lv_point_precise_t trail_points[TRAIL_MAX];
    int trail_count;

    /* what was typed (for the preview, capitals and word replacement) */
    char tail[TAIL_MAX];
    char last_word[SWIPE_MAX_WORD + 1]; /* the swiped word, as typed */
    word_case_t last_case;
    bool word_just_swiped;
    uint32_t last_space_tick;
    swipe_candidate_t candidates[SUGGESTIONS];
    int candidate_count;
    uint32_t warned_tick;
} keyboard_t;

static keyboard_t *s_kb;

/* --------------------------------------------------------------- text */

static int utf8_count(const char *s)
{
    int n = 0;

    for (; *s; s++)
    {
        n += ((unsigned char)*s & 0xC0) != 0x80;
    }

    return n;
}

/* Bytes of the last UTF-8 character of s (0 if empty). */
static size_t utf8_last_len(const char *s)
{
    size_t len = strlen(s);
    size_t i = len;

    while (i > 0)
    {
        i--;

        if (((unsigned char)s[i] & 0xC0) != 0x80)
        {
            return len - i;
        }
    }

    return 0;
}

static void tail_append(const char *text)
{
    size_t len = strlen(s_kb->tail);
    size_t add = strlen(text);

    if (len + add >= TAIL_MAX)
    {
        /* keep the second half, cut on a character boundary */
        size_t cut = len / 2;

        while (cut < len && ((unsigned char)s_kb->tail[cut] & 0xC0) == 0x80)
        {
            cut++;
        }

        memmove(s_kb->tail, s_kb->tail + cut, len - cut + 1);
        len -= cut;
    }

    if (add < TAIL_MAX - len)
    {
        memcpy(s_kb->tail + len, text, add + 1);
    }
}

static void tail_delete(int count)
{
    for (int i = 0; i < count; i++)
    {
        size_t last = utf8_last_len(s_kb->tail);

        if (last == 0)
        {
            return;
        }

        s_kb->tail[strlen(s_kb->tail) - last] = '\0';
    }
}

static char tail_last_char(void)
{
    size_t len = strlen(s_kb->tail);
    return len > 0 ? s_kb->tail[len - 1] : '\0';
}

/* Start of a sentence: nothing typed yet, a new line, or ". " "! " "? ". */
static bool at_sentence_start(void)
{
    size_t i = strlen(s_kb->tail);
    bool spaces = false;

    while (i > 0 && s_kb->tail[i - 1] == ' ')
    {
        spaces = true;
        i--;
    }

    if (i == 0)
    {
        return true;
    }

    char c = s_kb->tail[i - 1];

    return c == '\n' || (spaces && (c == '.' || c == '!' || c == '?'));
}

/* "ciao" -> "Ciao" / "CIAO" (ASCII letters; accented ones stay as they are). */
static void apply_case(char *word, word_case_t word_case)
{
    for (char *p = word; *p; p++)
    {
        if (word_case == CASE_ALL || (word_case == CASE_FIRST && p == word))
        {
            *p = (char)toupper((unsigned char)*p);
        }
    }
}

/* ------------------------------------------------------------- output */

static bool connected(void)
{
    if (hid_link_state() == HID_LINK_CONNECTED)
    {
        return true;
    }

    uint32_t now = lv_tick_get();

    if (now - s_kb->warned_tick > 2500)
    {
        s_kb->warned_tick = now;
        ui_toast(hid_link_transport() == HID_LINK_USB ? "Non connesso: collega il cavo USB"
                                                      : "Non connesso: associa l'orologio");
    }

    return false;
}

static void update_preview(void)
{
    const char *tail = s_kb->tail;

    if (tail[0] == '\0')
    {
        lv_label_set_text(s_kb->preview, hid_link_state() == HID_LINK_CONNECTED ? "Scrivi: il testo va al computer"
                                                                                : remote_status_text());
        lv_obj_set_style_text_color(s_kb->preview, ui_color(UI_COLOR_TEXT_DIM), 0);
        return;
    }

    /* the last ~20 characters, new lines shown as a small arrow */
    int skip = utf8_count(tail) - 20;
    const char *p = tail;

    while (skip > 0 && *p)
    {
        p++;

        while (((unsigned char)*p & 0xC0) == 0x80)
        {
            p++;
        }

        skip--;
    }

    char shown[96];
    size_t n = 0;

    for (; *p && n < sizeof(shown) - 8; p++)
    {
        if (*p == '\n')
        {
            memcpy(shown + n, " | ", 3);
            n += 3;
        }
        else
        {
            shown[n++] = *p;
        }
    }

    shown[n++] = '|';
    shown[n] = '\0';

    lv_label_set_text(s_kb->preview, shown);
    lv_obj_set_style_text_color(s_kb->preview, ui_color(UI_COLOR_TEXT), 0);
}

static void update_status(lv_timer_t *timer)
{
    (void)timer;
    hid_link_state_t state = hid_link_state();
    uint32_t color = state == HID_LINK_CONNECTED ? UI_COLOR_GREEN
                     : state == HID_LINK_WAITING ? UI_COLOR_ORANGE
                                                 : UI_COLOR_RED;

    lv_obj_set_style_bg_color(s_kb->dot, ui_color(color), 0);

    if (s_kb->tail[0] == '\0')
    {
        update_preview();
    }
}

static void refresh_keys(void);

/* After any text change: capital letter for the next sentence. */
static void text_changed(void)
{
    if (s_kb->shift != SHIFT_LOCK)
    {
        s_kb->shift = (remote_settings()->auto_caps && at_sentence_start()) ? SHIFT_ONCE : SHIFT_OFF;
    }

    update_preview();
    refresh_keys();
}

static void clear_suggestions(void)
{
    s_kb->candidate_count = 0;
    s_kb->word_just_swiped = false;

    for (int i = 0; i < SUGGESTIONS; i++)
    {
        lv_label_set_text(s_kb->suggest_label[i], "");
        lv_obj_set_style_bg_opa(s_kb->suggest[i], LV_OPA_TRANSP, 0);
    }
}

static void show_suggestions(int selected)
{
    for (int i = 0; i < SUGGESTIONS; i++)
    {
        if (i < s_kb->candidate_count)
        {
            char word[SWIPE_MAX_WORD + 1];
            strncpy(word, s_kb->candidates[i].word, SWIPE_MAX_WORD);
            word[SWIPE_MAX_WORD] = '\0';
            apply_case(word, s_kb->last_case);
            lv_label_set_text(s_kb->suggest_label[i], word);
        }
        else
        {
            lv_label_set_text(s_kb->suggest_label[i], "");
        }

        lv_obj_set_style_bg_opa(s_kb->suggest[i], i == selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

/* Type text, keep the preview in sync. Returns false if not connected. */
static bool send_text(const char *text)
{
    if (!connected())
    {
        return false;
    }

    if (!hid_link_type(text))
    {
        ui_toast("Invio non accettato: riprova");
        return false;
    }
    tail_append(text);
    return true;
}

static void type_key_text(const char *text)
{
    char out[8];
    strncpy(out, text, sizeof(out) - 1);
    out[sizeof(out) - 1] = '\0';

    if (s_kb->layer == LAYER_LETTERS && s_kb->shift != SHIFT_OFF)
    {
        apply_case(out, CASE_ALL);
    }

    /* a character the host layout cannot produce */
    const unsigned char *u = (const unsigned char *)out;
    uint32_t cp = u[0];

    if (cp >= 0xC0 && u[1])
    {
        cp = cp >= 0xE0 ? ((cp & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F)
                        : ((cp & 0x1F) << 6) | (u[1] & 0x3F);
    }

    if (!hid_link_can_type(cp))
    {
        ui_toast("Simbolo non disponibile con questo layout");
        return;
    }

    if (!send_text(out))
    {
        return;
    }

    clear_suggestions();
    text_changed();
}

static void type_special(uint8_t keycode)
{
    if (!connected())
    {
        return;
    }

    hid_link_key(0, keycode);
    clear_suggestions();
}

/* The finger traced a word. */
static void commit_swipe(void)
{
    const swipe_dict_t *dict = remote_dictionary();

    if (dict == NULL)
    {
        ui_toast("Dizionario non disponibile");
        return;
    }

    s_kb->candidate_count =
        swipe_decode(dict, &s_kb->layout, s_kb->path, s_kb->path_count, s_kb->candidates, SUGGESTIONS);

    if (s_kb->candidate_count == 0)
    {
        ui_toast("Parola non riconosciuta");
        return;
    }

    if (!connected())
    {
        s_kb->candidate_count = 0;
        return;
    }

    s_kb->last_case = s_kb->shift == SHIFT_LOCK ? CASE_ALL : s_kb->shift == SHIFT_ONCE ? CASE_FIRST : CASE_LOWER;

    /* Accept the optional space and word in one transaction. */
    char last = tail_last_char();
    char word[SWIPE_MAX_WORD + 1];
    strncpy(word, s_kb->candidates[0].word, SWIPE_MAX_WORD);
    word[SWIPE_MAX_WORD] = '\0';
    apply_case(word, s_kb->last_case);
    char output[SWIPE_MAX_WORD + 2];
    bool space = last != '\0' && last != ' ' && last != '\n' && last != '\'' && last != '(';
    snprintf(output, sizeof(output), "%s%s", space ? " " : "", word);
    if (!send_text(output))
    {
        clear_suggestions();
        return;
    }
    memcpy(s_kb->last_word, word, strlen(word) + 1);

    text_changed();
    show_suggestions(0);
    s_kb->word_just_swiped = true; /* after text_changed: a backspace now deletes the word */

    ESP_LOGD(TAG, "swipe: %s (%d points)", s_kb->last_word, s_kb->path_count);
}

/* A suggestion was tapped: swap the swiped word with it. */
static void on_suggestion(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    if (index >= s_kb->candidate_count || !s_kb->word_just_swiped || !connected())
    {
        return;
    }

    int length = utf8_count(s_kb->last_word);
    char word[SWIPE_MAX_WORD + 1];
    strncpy(word, s_kb->candidates[index].word, SWIPE_MAX_WORD);
    word[SWIPE_MAX_WORD] = '\0';
    apply_case(word, s_kb->last_case);
    if (!remote_send_ok(hid_link_replace_text(length, word)))
        return;
    tail_delete(length);
    tail_append(word);
    memcpy(s_kb->last_word, word, strlen(word) + 1);

    update_preview();
    show_suggestions(index);
}

/* ---------------------------------------------------------- key caps */

static void refresh_keys(void)
{
    bool upper = s_kb->layer == LAYER_LETTERS && s_kb->shift != SHIFT_OFF;

    for (int i = 0; i < KEYS; i++)
    {
        const key_def_t *key = &s_layers[s_kb->layer][i];
        const char *label = key->label != NULL ? key->label : key->text;

        if (upper && key->text != NULL && key->text[1] == '\0' && isalpha((unsigned char)key->text[0]))
        {
            char up[2] = {(char)toupper((unsigned char)key->text[0]), '\0'};
            lv_label_set_text(s_kb->cap_labels[i], up);
        }
        else
        {
            lv_label_set_text(s_kb->cap_labels[i], label);
        }

        lv_obj_set_style_text_font(s_kb->cap_labels[i],
                                   key->keycode != 0 && key->label[0] != '\xEF' ? UI_FONT_SMALL : UI_FONT_LARGE, 0);
    }

    /* control row: ⇧ is shift on letters, "#+=" / "123" elsewhere */
    if (s_kb->layer == LAYER_LETTERS)
    {
        lv_label_set_text(s_kb->shift_label, s_kb->shift == SHIFT_LOCK ? LV_SYMBOL_UP LV_SYMBOL_UP : LV_SYMBOL_UP);
        lv_obj_set_style_text_color(s_kb->shift_label, s_kb->shift == SHIFT_OFF ? ui_color(UI_COLOR_TEXT) : ui_accent(),
                                    0);
        lv_label_set_text(s_kb->layer_label, "123");
    }
    else
    {
        lv_label_set_text(s_kb->shift_label, s_kb->layer == LAYER_NUMBERS ? "#+=" : "123");
        lv_obj_set_style_text_color(s_kb->shift_label, ui_color(UI_COLOR_TEXT), 0);
        lv_label_set_text(s_kb->layer_label, "abc");
    }
}

static void highlight_key(int index, bool on)
{
    if (index >= 0 && index < KEYS)
    {
        lv_obj_set_style_bg_color(s_kb->caps[index], on ? ui_accent() : ui_color(UI_COLOR_CARD), 0);
    }
}

static void show_bubble(int index, const char *text)
{
    if (index < 0)
    {
        lv_obj_set_hidden(s_kb->bubble, true);
        return;
    }

    ui_key_rect_t key = s_kb->geometry.keys[index];
    int32_t bw = lv_obj_get_width(s_kb->bubble);
    int32_t y = s_kb->geometry.pad.y + key.y - 56;
    if (y < s_kb->geometry.header.y)
    {
        y = s_kb->geometry.header.y;
    }
    ui_key_rect_t band = ui_keyboard_band(s_kb->pad_w, board_info()->height, board_info()->round, y, 56, 6);
    int32_t x = key.x + key.w / 2 - bw / 2;

    if (x < band.x)
    {
        x = band.x;
    }

    if (x > band.x + band.w - bw)
    {
        x = band.x + band.w - bw;
    }

    /* LV_SYMBOL arrows exist only in the built-in fonts */
    lv_obj_set_style_text_font(s_kb->bubble_label, (unsigned char)text[0] == 0xEF ? UI_FONT_LARGE : ui_font_headline,
                               0);
    lv_label_set_text(s_kb->bubble_label, text);
    lv_obj_set_pos(s_kb->bubble, x, y < 0 ? 0 : y);
    lv_obj_set_hidden(s_kb->bubble, false);
}

/* What a key shows in the bubble / types now (case applied). */
static void key_text_now(int index, bool held, char *out, size_t size)
{
    const key_def_t *key = &s_layers[s_kb->layer][index];
    const char *text = key->text != NULL ? key->text : key->label;

    if (held && s_kb->layer == LAYER_LETTERS && accent_of(text) != NULL)
    {
        text = accent_of(text);
    }

    strncpy(out, text, size - 1);
    out[size - 1] = '\0';

    if (s_kb->layer == LAYER_LETTERS && s_kb->shift != SHIFT_OFF)
    {
        apply_case(out, CASE_ALL);
    }
}

/* ---------------------------------------------------------- the pad */

static int key_at(float x, float y)
{
    for (int i = 0; i < KEYS; i++)
    {
        ui_key_rect_t key = s_kb->geometry.keys[i];
        if (x >= key.x && x < key.x + key.w && y >= key.y && y < key.y + key.h)
        {
            return i;
        }
    }
    return -1;
}

static bool pad_point(float *x, float *y)
{
    lv_indev_t *indev = lv_indev_active();

    if (indev == NULL)
    {
        return false;
    }

    lv_point_t p;
    lv_area_t area;
    lv_indev_get_point(indev, &p);
    lv_obj_get_coords(s_kb->pad, &area);

    float px = (float)(p.x - area.x1);
    float py = (float)(p.y - area.y1);

    /* the finger may slide out of the pad: clamp to its border */
    *x = px < 0 ? 0 : px > s_kb->pad_w - 1 ? (float)(s_kb->pad_w - 1) : px;
    *y = py < 0 ? 0 : py > s_kb->pad_h - 1 ? (float)(s_kb->pad_h - 1) : py;
    return true;
}

static void path_add(float x, float y)
{
    keyboard_t *kb = s_kb;

    if (kb->path_count > 0)
    {
        swipe_point_t *last = &kb->path[kb->path_count - 1];
        float dx = x - last->x;
        float dy = y - last->y;
        float d = sqrtf(dx * dx + dy * dy);

        if (d < kb->path_step)
        {
            return;
        }

        kb->path_length += d;
    }

    if (kb->path_count == SWIPE_MAX_POINTS)
    {
        /* very long word: keep every other point, sample less often */
        for (int i = 0; i < SWIPE_MAX_POINTS / 2; i++)
        {
            kb->path[i] = kb->path[i * 2];
        }

        kb->path_count = SWIPE_MAX_POINTS / 2;
        kb->path_step *= 2.0f;
    }

    kb->path[kb->path_count++] = (swipe_point_t){x, y};

    /* the visible trail: recent points, a bit sparser */
    if (kb->trail_count > 0)
    {
        lv_point_precise_t *t = &kb->trail_points[kb->trail_count - 1];
        float dx = x - (float)t->x;
        float dy = y - (float)t->y;

        if (dx * dx + dy * dy < 36.0f)
        {
            return;
        }
    }

    if (kb->trail_count == TRAIL_MAX)
    {
        memmove(kb->trail_points, kb->trail_points + 1, sizeof(kb->trail_points[0]) * (TRAIL_MAX - 1));
        kb->trail_count--;
    }

    kb->trail_points[kb->trail_count++] = (lv_point_precise_t){(lv_value_precise_t)x, (lv_value_precise_t)y};
}

/* Is this touch a swipe (several letters) rather than a tap? */
static bool looks_like_swipe(void)
{
    const remote_settings_t *settings = remote_settings();

    if (s_kb->layer != LAYER_LETTERS || !settings->swipe)
    {
        return false;
    }

    return (s_kb->distinct_keys >= 2 && s_kb->path_length >= 0.75f * s_kb->key_w) || s_kb->distinct_keys >= 3;
}

static void pad_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    keyboard_t *kb = s_kb;
    float x;
    float y;

    if (kb == NULL)
    {
        return; /* screen being deleted */
    }

    if (code == LV_EVENT_PRESSED)
    {
        if (!pad_point(&x, &y))
        {
            return;
        }

        kb->touching = true;
        kb->swiping = false;
        kb->path_count = 0;
        kb->path_length = 0;
        kb->path_step = 3.0f;
        kb->trail_count = 0;
        kb->down_tick = lv_tick_get();
        kb->down_key = key_at(x, y);
        if (kb->down_key < 0)
        {
            kb->touching = false;
            return;
        }
        kb->last_key = kb->down_key;
        kb->distinct_keys = 1;
        path_add(x, y);

        char text[8];
        key_text_now(kb->down_key, false, text, sizeof(text));
        highlight_key(kb->down_key, true);
        show_bubble(kb->down_key, text);
    }
    else if (code == LV_EVENT_PRESSING && kb->touching)
    {
        if (!pad_point(&x, &y))
        {
            return;
        }

        path_add(x, y);
        int key = key_at(x, y);

        if (key != kb->last_key && key >= 0)
        {
            kb->distinct_keys++;
            kb->last_key = key;
        }

        if (!kb->swiping && looks_like_swipe())
        {
            kb->swiping = true;
            highlight_key(kb->down_key, false);
            show_bubble(-1, NULL);
            lv_obj_set_hidden(kb->trail, false);
        }

        if (kb->swiping)
        {
            lv_line_set_points(kb->trail, kb->trail_points, (uint32_t)kb->trail_count);
        }
        else if (kb->layer != LAYER_LETTERS && key != kb->down_key && key >= 0)
        {
            /* numbers / symbols: slide to correct, the key under the finger counts */
            highlight_key(kb->down_key, false);
            kb->down_key = key;
            highlight_key(key, true);

            char text[8];
            key_text_now(key, false, text, sizeof(text));
            show_bubble(key, text);
        }
        else if (lv_tick_elaps(kb->down_tick) >= HOLD_MS && kb->down_key >= 0)
        {
            char text[8];
            key_text_now(kb->down_key, true, text, sizeof(text));
            lv_label_set_text(kb->bubble_label, text);
        }
    }
    else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && kb->touching)
    {
        kb->touching = false;
        highlight_key(kb->down_key, false);
        show_bubble(-1, NULL);
        lv_obj_set_hidden(kb->trail, true);

        if (code == LV_EVENT_PRESS_LOST)
        {
            return;
        }

        if (kb->swiping)
        {
            commit_swipe();
            return;
        }

        if (kb->down_key < 0)
        {
            return;
        }

        const key_def_t *key = &s_layers[kb->layer][kb->down_key];

        if (key->keycode != 0)
        {
            type_special(key->keycode);
            return;
        }

        char text[8];
        key_text_now(kb->down_key, lv_tick_elaps(kb->down_tick) >= HOLD_MS, text, sizeof(text));
        type_key_text(text);
    }
}

/* ----------------------------------------------------- control row */

static void on_shift(lv_event_t *e)
{
    (void)e;

    if (s_kb->layer == LAYER_LETTERS)
    {
        s_kb->shift = s_kb->shift == SHIFT_OFF ? SHIFT_ONCE : s_kb->shift == SHIFT_ONCE ? SHIFT_LOCK : SHIFT_OFF;
    }
    else
    {
        s_kb->layer = s_kb->layer == LAYER_NUMBERS ? LAYER_SYMBOLS : LAYER_NUMBERS;
    }

    refresh_keys();
}

static void on_layer(lv_event_t *e)
{
    (void)e;
    s_kb->layer = s_kb->layer == LAYER_LETTERS ? LAYER_NUMBERS : LAYER_LETTERS;
    refresh_keys();
}

static void on_apostrophe(lv_event_t *e)
{
    (void)e;
    type_key_text("'");
}

static void on_space(lv_event_t *e)
{
    (void)e;

    if (!connected())
    {
        return;
    }

    /* space space -> ". " after a word */
    size_t len = strlen(s_kb->tail);
    bool double_space = len >= 2 && s_kb->tail[len - 1] == ' ' &&
                        (isalnum((unsigned char)s_kb->tail[len - 2]) || (unsigned char)s_kb->tail[len - 2] >= 0x80) &&
                        lv_tick_elaps(s_kb->last_space_tick) < DOUBLE_SPACE_MS;

    if (double_space)
    {
        if (!remote_send_ok(hid_link_replace_text(1, ". ")))
            return;
        tail_delete(1);
        tail_append(". ");
        s_kb->last_space_tick = 0;
    }
    else
    {
        if (!send_text(" "))
            return;
        s_kb->last_space_tick = lv_tick_get();
    }

    clear_suggestions();
    text_changed();
}

static void backspace_once(void)
{
    if (!connected())
    {
        return;
    }

    if (s_kb->word_just_swiped)
    {
        int length = utf8_count(s_kb->last_word);
        if (!remote_send_ok(hid_link_backspace(length)))
            return;
        tail_delete(length);
    }
    else
    {
        if (!remote_send_ok(hid_link_backspace(1)))
            return;
        tail_delete(1);
    }

    clear_suggestions();
    text_changed();
}

static void on_backspace(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT)
    {
        backspace_once();
    }
}

static void on_enter(lv_event_t *e)
{
    (void)e;

    if (!connected())
    {
        return;
    }

    if (!remote_send_ok(hid_link_key(0, HIDL_KEY_ENTER)))
        return;
    tail_append("\n");
    clear_suggestions();
    text_changed();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    app_back();
}

/* --------------------------------------------------------------- UI */

/* Each control receives its own band, including the two bottom rows on circles. */
/* Each control receives its own band, including the two bottom rows on circles. */
static lv_obj_t *control_button(lv_obj_t *parent, ui_key_rect_t rect, const char *text, lv_event_cb_t cb,
                                lv_event_code_t code, uint32_t color)
{
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, rect.x + 2, rect.y + 2);
    lv_obj_set_size(button, rect.w - 4, rect.h - 4);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, ui_color(color), 0);
    lv_obj_set_style_bg_color(button, ui_accent(), LV_STATE_PRESSED);
    lv_obj_set_scrollable(button, false);
    lv_obj_add_event_cb(button, cb, code, NULL);
    lv_port_set_focusable(button);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
    lv_obj_center(label);
    return label;
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    s_kb = calloc(1, sizeof(keyboard_t));
    if (s_kb == NULL)
    {
        ui_toast("Memoria insufficiente");
        return;
    }
    keyboard_t *kb = s_kb;
    int32_t width = lv_display_get_horizontal_resolution(lv_display_get_default());
    int32_t height = lv_display_get_vertical_resolution(lv_display_get_default());
    ui_keyboard_layout_init(&kb->geometry, width, height, board_info()->round);
    kb->screen = screen;
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    ui_key_rect_t header = kb->geometry.header;
    lv_obj_t *back = lv_obj_create(screen);
    lv_obj_remove_style_all(back);
    lv_obj_set_pos(back, header.x, header.y);
    lv_obj_set_size(back, 32, header.h);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_port_set_focusable(back);
    lv_obj_t *arrow = lv_label_create(back);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(arrow, UI_FONT_BODY, 0);
    lv_obj_center(arrow);

    kb->preview = lv_label_create(screen);
    lv_obj_set_size(kb->preview, header.w - 52, header.h);
    lv_label_set_long_mode(kb->preview, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(kb->preview, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_align(kb->preview, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(kb->preview, header.x + 34, header.y + (header.h - 20) / 2);
    kb->dot = lv_obj_create(screen);
    lv_obj_remove_style_all(kb->dot);
    lv_obj_set_size(kb->dot, 10, 10);
    lv_obj_set_style_radius(kb->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(kb->dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(kb->dot, header.x + header.w - 14, header.y + (header.h - 10) / 2);
    ui_key_rect_t suggestions = kb->geometry.suggestions;
    for (int i = 0; i < SUGGESTIONS; i++)
    {
        int32_t x0 = suggestions.w * i / SUGGESTIONS;
        int32_t x1 = suggestions.w * (i + 1) / SUGGESTIONS;
        lv_obj_t *s = lv_obj_create(screen);
        lv_obj_remove_style_all(s);
        lv_obj_set_pos(s, suggestions.x + x0 + 2, suggestions.y);
        lv_obj_set_size(s, x1 - x0 - 4, suggestions.h);
        lv_obj_set_style_radius(s, 10, 0);
        lv_obj_set_style_bg_color(s, ui_color(UI_COLOR_CARD_HI), 0);
        lv_obj_set_style_bg_color(s, ui_accent(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_scrollable(s, false);
        lv_obj_add_event_cb(s, on_suggestion, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_port_set_focusable(s);
        lv_obj_t *label = lv_label_create(s);
        lv_obj_set_width(label, x1 - x0 - 8);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
        lv_obj_center(label);
        kb->suggest[i] = s;
        kb->suggest_label[i] = label;
    }
    kb->pad_w = width;
    kb->pad_h = kb->geometry.pad.h;
    kb->key_w = (float)width / COLS;
    kb->key_h = (float)kb->pad_h / ROWS;
    kb->layout.key_w = kb->geometry.keys[0].w;
    kb->layout.key_h = kb->key_h;
    kb->pad = lv_obj_create(screen);
    lv_obj_remove_style_all(kb->pad);
    lv_obj_set_pos(kb->pad, 0, kb->geometry.pad.y);
    lv_obj_set_size(kb->pad, width, kb->pad_h);
    lv_obj_set_scrollable(kb->pad, false);
    lv_obj_set_clickable(kb->pad, true);
    lv_obj_set_press_lock(kb->pad, true);
    lv_obj_set_gesture_bubble(kb->pad, false);
    lv_obj_add_event_cb(kb->pad, pad_event, LV_EVENT_ALL, NULL);
    for (int i = 0; i < KEYS; i++)
    {
        ui_key_rect_t rect = kb->geometry.keys[i];
        lv_obj_t *cap = lv_obj_create(kb->pad);
        lv_obj_remove_style_all(cap);
        lv_obj_set_pos(cap, rect.x + 2, rect.y + 2);
        lv_obj_set_size(cap, rect.w - 4, rect.h - 4);
        lv_obj_set_style_radius(cap, 10, 0);
        lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(cap, ui_color(UI_COLOR_CARD), 0);
        lv_obj_set_clickable(cap, false);
        lv_obj_set_scrollable(cap, false);
        lv_obj_t *label = lv_label_create(cap);
        lv_obj_center(label);
        kb->caps[i] = cap;
        kb->cap_labels[i] = label;
        const char *text = s_layers[LAYER_LETTERS][i].text;
        if (text != NULL && text[0] >= 'a' && text[0] <= 'z')
        {
            kb->layout.center[text[0] - 'a'] = (swipe_point_t){rect.x + rect.w / 2.0f, rect.y + rect.h / 2.0f};
        }
    }
    kb->key_w = kb->layout.key_w; /* gesture threshold tracks the actual narrow keys */
    kb->trail = lv_line_create(kb->pad);
    lv_obj_set_size(kb->trail, width, kb->pad_h);
    lv_obj_set_style_line_width(kb->trail, 6, 0);
    lv_obj_set_style_line_rounded(kb->trail, true, 0);
    lv_obj_set_style_line_color(kb->trail, ui_accent(), 0);
    lv_obj_set_style_line_opa(kb->trail, LV_OPA_80, 0);
    lv_obj_set_clickable(kb->trail, false);
    lv_obj_set_hidden(kb->trail, true);
    ui_key_rect_t *controls = kb->geometry.controls;
    kb->shift_label = control_button(screen, controls[0], LV_SYMBOL_UP, on_shift, LV_EVENT_CLICKED, UI_COLOR_CARD_HI);
    kb->layer_label = control_button(screen, controls[1], "123", on_layer, LV_EVENT_CLICKED, UI_COLOR_CARD_HI);
    control_button(screen, controls[2], "'", on_apostrophe, LV_EVENT_CLICKED, UI_COLOR_CARD_HI);
    control_button(screen, controls[3], "spazio", on_space, LV_EVENT_CLICKED, UI_COLOR_CARD);
    lv_obj_t *bs =
        control_button(screen, controls[4], LV_SYMBOL_BACKSPACE, on_backspace, LV_EVENT_PRESSED, UI_COLOR_CARD_HI);
    lv_obj_add_event_cb(lv_obj_get_parent(bs), on_backspace, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    control_button(screen, controls[5], LV_SYMBOL_NEW_LINE, on_enter, LV_EVENT_CLICKED, UI_COLOR_BLUE);
    kb->bubble = lv_obj_create(screen);
    lv_obj_remove_style_all(kb->bubble);
    lv_obj_set_size(kb->bubble, 56, 56);
    lv_obj_set_style_radius(kb->bubble, 14, 0);
    lv_obj_set_style_bg_opa(kb->bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(kb->bubble, ui_color(UI_COLOR_CARD_HI), 0);
    lv_obj_set_style_border_width(kb->bubble, 2, 0);
    lv_obj_set_style_border_color(kb->bubble, ui_accent(), 0);
    lv_obj_set_clickable(kb->bubble, false);
    lv_obj_set_hidden(kb->bubble, true);
    kb->bubble_label = lv_label_create(kb->bubble);
    lv_obj_set_style_text_font(kb->bubble_label, ui_font_headline, 0);
    lv_obj_center(kb->bubble_label);
    clear_suggestions();
    text_changed();
    update_status(NULL);
    kb->status_timer = lv_timer_create(update_status, 300, NULL);
}

static void destroy(void)
{
    if (s_kb != NULL)
    {
        lv_timer_delete(s_kb->status_timer);
        free(s_kb);
        s_kb = NULL;
    }
}

const app_t keyboard_app = {
    .id = "remote.keyboard",
    .name = "Tastiera",
    .icon = LV_SYMBOL_KEYBOARD,
    .color = UI_COLOR_BLUE,
    .flags = APP_FLAG_HIDDEN | APP_FLAG_NO_BACK_GESTURE | APP_FLAG_KEEP_SCREEN_ON,
    .create = create,
    .destroy = destroy,
};
