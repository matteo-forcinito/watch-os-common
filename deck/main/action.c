/* Deck action parser (see action.h). Freestanding: no libc. */
#include "action.h"

typedef struct
{
    const char *name;
    uint8_t code;
} name_code_t;

typedef struct
{
    const char *name;
    uint16_t usage;
} name_usage_t;

static const name_code_t s_modifiers[] = {
    {"ctrl", 0x01}, {"control", 0x01}, {"shift", 0x02}, {"alt", 0x04}, {"win", 0x08},
    {"gui", 0x08},  {"cmd", 0x08},     {"meta", 0x08},  {"altgr", 0x40},
};

static const name_code_t s_keys[] = {
    {"enter", 0x28},     {"return", 0x28},   {"esc", 0x29},       {"escape", 0x29},    {"backspace", 0x2A},
    {"tab", 0x2B},       {"space", 0x2C},    {"minus", 0x2D},     {"equal", 0x2E},     {"lbracket", 0x2F},
    {"rbracket", 0x30},  {"backslash", 0x31}, {"semicolon", 0x33}, {"quote", 0x34},     {"grave", 0x35},
    {"comma", 0x36},     {"period", 0x37},   {"dot", 0x37},       {"slash", 0x38},     {"capslock", 0x39},
    {"printscreen", 0x46}, {"scrolllock", 0x47}, {"pause", 0x48},  {"insert", 0x49},    {"home", 0x4A},
    {"pageup", 0x4B},    {"delete", 0x4C},   {"del", 0x4C},       {"end", 0x4D},       {"pagedown", 0x4E},
    {"right", 0x4F},     {"left", 0x50},     {"down", 0x51},      {"up", 0x52},        {"kpminus", 0x56},
    {"kpplus", 0x57},    {"menu", 0x65},
};

static const name_usage_t s_media[] = {
    {"play", 0x00CD}, {"next", 0x00B5}, {"prev", 0x00B6}, {"previous", 0x00B6}, {"stop", 0x00B7},
    {"mute", 0x00E2}, {"volup", 0x00E9}, {"voldown", 0x00EA}, {"calc", 0x0192}, {"mail", 0x018A},
    {"home", 0x0223}, {"search", 0x0221},
};

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

/* Word [start, end) equals name (case-insensitive)? */
static bool word_is(const char *start, const char *end, const char *name)
{
    while (start < end && *name && lower(*start) == *name)
    {
        start++;
        name++;
    }
    return start == end && *name == 0;
}

static bool starts_with(const char *text, const char *end, const char *prefix)
{
    while (*prefix && text < end && lower(*text) == *prefix)
    {
        text++;
        prefix++;
    }
    return *prefix == 0;
}

static bool number(const char *start, const char *end, int *out)
{
    bool negative = false;
    int value = 0, digits = 0;
    if (start < end && (*start == '-' || *start == '+'))
        negative = *start++ == '-';
    for (; start < end && *start >= '0' && *start <= '9'; start++, digits++)
        value = value * 10 + (*start - '0');
    if (start != end || digits == 0 || digits > 5)
        return false;
    *out = negative ? -value : value;
    return true;
}

/* One key name -> HID usage (0 if unknown). */
static uint8_t key_code(const char *start, const char *end)
{
    int length = (int)(end - start);
    if (length == 1)
    {
        char c = lower(*start);
        if (c >= 'a' && c <= 'z')
            return (uint8_t)(0x04 + c - 'a');
        if (c >= '1' && c <= '9')
            return (uint8_t)(0x1E + c - '1');
        if (c == '0')
            return 0x27;
    }
    if (length >= 2 && length <= 3 && lower(*start) == 'f')
    {
        int n = 0;
        if (number(start + 1, end, &n) && n >= 1 && n <= 24)
            return (uint8_t)(n <= 12 ? 0x3A + n - 1 : 0x68 + n - 13);
    }
    for (unsigned i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++)
        if (word_is(start, end, s_keys[i].name))
            return s_keys[i].code;
    return 0;
}

static const char *trim_start(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    return p;
}

static const char *trim_end(const char *start, const char *p)
{
    while (p > start && (p[-1] == ' ' || p[-1] == '\t'))
        p--;
    return p;
}

static bool fail(deck_action_t *out, const char *base, const char *at, const char *message)
{
    out->error_at = (int)(at - base);
    out->error = message;
    return false;
}

static bool add(deck_action_t *out, deck_step_t step, const char *base, const char *at)
{
    if (out->count >= DECK_STEPS)
        return fail(out, base, at, "Troppi passi (massimo 24)");
    out->steps[out->count++] = step;
    return true;
}

/* "ctrl+shift+m" -> one KEY step. */
static bool chord(deck_action_t *out, const char *base, const char *start, const char *end)
{
    deck_step_t step = {.kind = DECK_STEP_KEY};
    const char *p = start;
    while (p < end)
    {
        const char *q = p;
        while (q < end && *q != '+')
            q++;
        if (q == p)
            return fail(out, base, p, "Tasto mancante prima di '+'");
        bool modifier = false;
        for (unsigned i = 0; i < sizeof(s_modifiers) / sizeof(s_modifiers[0]); i++)
            if (word_is(p, q, s_modifiers[i].name))
            {
                step.modifiers |= s_modifiers[i].code;
                modifier = true;
            }
        if (!modifier)
        {
            if (step.key)
                return fail(out, base, p, "Un solo tasto per combinazione");
            step.key = key_code(p, q);
            if (step.key == 0)
                return fail(out, base, p, "Tasto sconosciuto");
        }
        p = q < end ? q + 1 : q;
        if (q < end && p == end)
            return fail(out, base, q, "Tasto mancante dopo '+'");
    }
    return add(out, step, base, start);
}

bool deck_action_parse(const char *text, deck_action_t *out)
{
    out->count = 0;
    out->error_at = -1;
    out->error = 0;
    if (text == 0)
        return fail(out, "", "", "Azione vuota");
    const char *end = text;
    while (*end)
        end++;
    if (end - text > 1000)
        return fail(out, text, text, "Azione troppo lunga");
    const char *p = text;
    while (p < end)
    {
        const char *step_end = p;
        while (step_end < end && *step_end != ';')
            step_end++;
        const char *s = trim_start(p, step_end), *e = trim_end(s, step_end);
        if (s < e)
        {
            if (starts_with(s, e, "type:"))
            {
                const char *t = s + 5;
                /* Text keeps its own spaces: only what follows "type:" up to ';'. */
                deck_step_t step = {.kind = DECK_STEP_TEXT, .offset = (uint16_t)(t - text),
                                    .length = (uint16_t)(step_end - t)};
                if (step.length == 0)
                    return fail(out, text, s, "Testo vuoto dopo type:");
                if (!add(out, step, text, s))
                    return false;
            }
            else if (starts_with(s, e, "media:"))
            {
                deck_step_t step = {.kind = DECK_STEP_MEDIA};
                for (unsigned i = 0; i < sizeof(s_media) / sizeof(s_media[0]); i++)
                    if (word_is(s + 6, e, s_media[i].name))
                        step.value = s_media[i].usage;
                if (step.value == 0)
                    return fail(out, text, s + 6, "Tasto media sconosciuto");
                if (!add(out, step, text, s))
                    return false;
            }
            else if (starts_with(s, e, "wait:"))
            {
                int ms = 0;
                if (!number(s + 5, e, &ms) || ms < 0 || ms > 5000)
                    return fail(out, text, s + 5, "Attesa tra 0 e 5000 ms");
                deck_step_t step = {.kind = DECK_STEP_WAIT, .value = (uint16_t)ms};
                if (!add(out, step, text, s))
                    return false;
            }
            else if (starts_with(s, e, "wheel:"))
            {
                int clicks = 0;
                if (!number(s + 6, e, &clicks) || clicks < -20 || clicks > 20 || clicks == 0)
                    return fail(out, text, s + 6, "Rotella tra -20 e 20");
                deck_step_t step = {.kind = DECK_STEP_WHEEL, .wheel = (int8_t)clicks};
                if (!add(out, step, text, s))
                    return false;
            }
            else
            {
                /* Chords separated by spaces. */
                const char *c = s;
                while (c < e)
                {
                    const char *ce = c;
                    while (ce < e && *ce != ' ')
                        ce++;
                    if (!chord(out, text, c, ce))
                        return false;
                    c = trim_start(ce, e);
                }
            }
        }
        p = step_end < end ? step_end + 1 : step_end;
    }
    if (out->count == 0)
        return fail(out, text, text, "Azione vuota");
    return true;
}
