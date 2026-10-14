/* Text helpers for News (see text.h). Pure C: also built by the host tests. */
#include "text.h"

#include <ctype.h>
#include <string.h>

/* ----------------------------------------------------------------- utf-8 */

static size_t utf8_put(char *out, uint32_t cp)
{
    if (cp < 0x80)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

void nw_copy(char *out, size_t size, const char *in)
{
    if (size == 0)
    {
        return;
    }
    size_t n = strlen(in);
    if (n >= size)
    {
        n = size - 1;
        while (n > 0 && ((unsigned char)in[n] & 0xC0) == 0x80)
        {
            n--;   /* do not cut a character in two */
        }
    }
    memcpy(out, in, n);
    out[n] = '\0';
}

uint32_t nw_hash(const char *text)
{
    uint32_t h = 2166136261u;
    for (; *text; text++)
    {
        h = (h ^ (uint8_t)*text) * 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------- entities */

typedef struct
{
    const char *name;
    uint16_t cp;
} entity_t;

static const entity_t s_entities[] = {
    {"amp", '&'},      {"lt", '<'},       {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
    {"nbsp", ' '},     {"agrave", 0xE0},  {"aacute", 0xE1},  {"egrave", 0xE8},  {"eacute", 0xE9},
    {"igrave", 0xEC},  {"iacute", 0xED},  {"ograve", 0xF2},  {"oacute", 0xF3},  {"ugrave", 0xF9},
    {"uacute", 0xFA},  {"Agrave", 0xC0},  {"Egrave", 0xC8},  {"Eacute", 0xC9},  {"Igrave", 0xCC},
    {"Ograve", 0xD2},  {"Ugrave", 0xD9},  {"ccedil", 0xE7},  {"ntilde", 0xF1},  {"ouml", 0xF6},
    {"uuml", 0xFC},    {"auml", 0xE4},    {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"rdquo", 0x201D},
    {"ldquo", 0x201C}, {"hellip", 0x2026}, {"ndash", 0x2013}, {"mdash", 0x2014}, {"laquo", 0xAB},
    {"raquo", 0xBB},   {"deg", 0xB0},     {"euro", 0x20AC},  {"middot", 0xB7},  {"bull", 0x2022},
};

/* `p` points after '&'. Writes the character, returns the bytes consumed (0 = not an entity). */
static size_t entity(const char *p, char *out, size_t *written)
{
    const char *semi = memchr(p, ';', 10);
    if (semi == NULL || semi == p)
    {
        return 0;
    }
    size_t len = (size_t)(semi - p);
    uint32_t cp = 0;
    if (p[0] == '#')
    {
        bool hex = p[1] == 'x' || p[1] == 'X';
        for (const char *d = p + (hex ? 2 : 1); d < semi; d++)
        {
            int v = isdigit((unsigned char)*d) ? *d - '0'
                    : hex && isxdigit((unsigned char)*d) ? (tolower((unsigned char)*d) - 'a' + 10) : -1;
            if (v < 0 || cp > 0x10FFFF)
            {
                return 0;
            }
            cp = cp * (hex ? 16 : 10) + (uint32_t)v;
        }
        if (cp == 0 || cp > 0x10FFFF)
        {
            return 0;
        }
    }
    else
    {
        for (size_t i = 0; i < sizeof(s_entities) / sizeof(s_entities[0]); i++)
        {
            if (strlen(s_entities[i].name) == len && strncmp(s_entities[i].name, p, len) == 0)
            {
                cp = s_entities[i].cp;
                break;
            }
        }
        if (cp == 0)
        {
            return 0;
        }
    }
    /* Typographic quotes and dashes become plain ones: simpler fonts, simpler keys. */
    if (cp == 0x2019 || cp == 0x2018)
    {
        cp = '\'';
    }
    else if (cp == 0x201C || cp == 0x201D || cp == 0xAB || cp == 0xBB)
    {
        cp = '"';
    }
    else if (cp == 0x2013 || cp == 0x2014)
    {
        cp = '-';
    }
    *written = utf8_put(out, cp);
    return len + 1;
}

size_t nw_clean(char *text)
{
    char *w = text;
    const char *r = text;
    bool space = true;   /* drops leading spaces */
    int depth = 0;       /* inside a tag */
    /* Two passes would be cleaner; one keeps it allocation free. Entities first
     * can produce '<' that are text, so tags are dropped before decoding. */
    while (*r)
    {
        if (strncmp(r, "<![CDATA[", 9) == 0)
        {
            r += 9;
            continue;
        }
        if (strncmp(r, "]]>", 3) == 0)
        {
            r += 3;
            continue;
        }
        if (*r == '<')
        {
            depth = 1;
            r++;
            continue;
        }
        if (depth)
        {
            if (*r == '>')
            {
                depth = 0;
                if (!space)
                {
                    *w++ = ' ';
                    space = true;
                }
            }
            r++;
            continue;
        }
        char decoded[4];
        size_t n = 0;
        size_t used = *r == '&' ? entity(r + 1, decoded, &n) : 0;
        if (used > 0)
        {
            r += used + 1;
            for (size_t i = 0; i < n; i++)
            {
                bool white = decoded[i] == ' ';
                if (white && space)
                {
                    continue;
                }
                *w++ = decoded[i];
                space = white;
            }
            continue;
        }
        char c = *r++;
        bool white = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (white)
        {
            if (!space)
            {
                *w++ = ' ';
                space = true;
            }
            continue;
        }
        *w++ = c;
        space = false;
    }
    while (w > text && w[-1] == ' ')
    {
        w--;
    }
    *w = '\0';
    return (size_t)(w - text);
}

/* -------------------------------------------------------------- keywords */

static const char *const s_stop[] = {
    "della", "delle", "dello", "degli", "dalla", "dalle", "dallo", "dagli", "nella", "nelle", "nello", "negli",
    "sulla", "sulle", "sullo", "sugli", "alla", "alle", "allo", "agli", "questo", "questa", "questi", "queste",
    "quello", "quella", "quelli", "quelle", "sono", "stato", "stata", "stati", "state", "essere", "come",
    "anche", "ancora", "dopo", "prima", "oggi", "ieri", "domani", "sempre", "tutto", "tutti", "tutte", "tutta",
    "molto", "molti", "molte", "dove", "quando", "perche", "mentre", "senza", "contro", "verso", "oltre", "fino",
    "circa", "sotto", "sopra", "presso", "secondo", "altro", "altri", "altra", "altre", "ogni", "nuovo", "nuova",
    "nuovi", "nuove", "fare", "fatto", "fatti", "dice", "detto", "dire", "solo", "proprio", "cosa", "cose",
    "loro", "nostro", "nostra", "vostro", "suoi", "hanno", "avere", "aveva", "viene", "vengono", "video", "foto",
    "diretta", "live", "news", "ultime", "ultima", "ultimo", "notizie", "anni", "anno", "giorni", "giorno", "ecco",
    "quali", "quale", "sara", "sarebbe", "puo", "possono", "deve", "devono", "vuole", "parla", "spiega",
    "intervista", "commento", "ansa", "corriere", "repubblica", "wired", "focus", "with", "from", "that", "this",
    "have", "about", "dell", "dall", "nell", "sull", "tanto", "poco", "meglio", "peggio", "grande", "grandi",
    "primo", "prime", "primi", "seconda", "terzo", "volta", "volte", "mesi", "mese", "settimana", "ore", "minuti",
    "ecco", "caso", "parte", "punto", "tempo", "modo", "storia", "italia", "italiano", "italiana", "italiani",
};

static bool stop_word(const char *folded)
{
    for (size_t i = 0; i < sizeof(s_stop) / sizeof(s_stop[0]); i++)
    {
        if (strcmp(s_stop[i], folded) == 0)
        {
            return true;
        }
    }
    return false;
}

/* One UTF-8 letter folded to ASCII lower case; 0 if not a letter or digit.
 * *len gets the bytes read; *upper whether it was a capital. */
static char fold(const unsigned char *p, int *len, bool *upper)
{
    *len = 1;
    *upper = false;
    if (p[0] < 0x80)
    {
        if (isalnum(p[0]))
        {
            *upper = isupper(p[0]) != 0;
            return (char)tolower(p[0]);
        }
        return 0;
    }
    if (p[0] == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF)
    {
        *len = 2;
        unsigned c = p[1] + 0x40;   /* Latin-1 code */
        *upper = c < 0xE0;
        c |= 0x20;
        if (c >= 0xE0 && c <= 0xE5) return 'a';
        if (c == 0xE7) return 'c';
        if (c >= 0xE8 && c <= 0xEB) return 'e';
        if (c >= 0xEC && c <= 0xEF) return 'i';
        if (c == 0xF1) return 'n';
        if (c >= 0xF2 && c <= 0xF6) return 'o';
        if (c >= 0xF9 && c <= 0xFC) return 'u';
        return 0;
    }
    /* Other multi-byte characters end a word. */
    *len = (p[0] & 0xE0) == 0xC0 ? 2 : (p[0] & 0xF0) == 0xE0 ? 3 : (p[0] & 0xF8) == 0xF0 ? 4 : 1;
    return 0;
}

static uint32_t stem_hash(const char *folded)
{
    char stem[7];
    size_t n = strlen(folded);
    n = n > 6 ? 6 : n;
    memcpy(stem, folded, n);
    stem[n] = '\0';
    return nw_hash(stem);
}

/* Reads the next word: folded text in `folded`, the original in `word`.
 * Returns the pointer after it, NULL at the end. */
static const char *next_word(const char *p, char *folded, size_t fsize, char *word, size_t wsize, int *capitals,
                             int *letters, bool *digits_only)
{
    const unsigned char *u = (const unsigned char *)p;
    int len;
    bool upper;
    while (*u && fold(u, &len, &upper) == 0)
    {
        u += len;
    }
    if (*u == '\0')
    {
        return NULL;
    }
    size_t f = 0, w = 0;
    *capitals = 0;
    *letters = 0;
    *digits_only = true;
    char c;
    while (*u && (c = fold(u, &len, &upper)) != 0)
    {
        if (f + 1 < fsize)
        {
            folded[f++] = c;
        }
        if (w + (size_t)len < wsize)
        {
            for (int i = 0; i < len; i++)
            {
                word[w++] = (char)u[i];
            }
        }
        (*letters)++;
        *capitals += upper;
        *digits_only &= isdigit((unsigned char)c) != 0;
        u += len;
    }
    folded[f] = '\0';
    word[w] = '\0';
    /* Lower case the original word (ASCII and Latin-1 capitals). */
    for (size_t i = 0; i < w; i++)
    {
        unsigned char *b = (unsigned char *)&word[i];
        if (b[0] == 0xC3 && i + 1 < w && b[1] >= 0x80 && b[1] <= 0x9E && b[1] != 0x97)
        {
            b[1] |= 0x20;
            i++;
        }
        else if (b[0] < 0x80)
        {
            b[0] = (unsigned char)tolower(b[0]);
        }
    }
    return (const char *)u;
}

int nw_keywords(const char *title, nw_key_t *out, int max)
{
    int count = 0;
    const char *p = title;
    char folded[40], word[NW_WORD];
    int capitals, letters;
    bool digits;
    while (count < max && (p = next_word(p, folded, sizeof(folded), word, sizeof(word), &capitals, &letters, &digits)))
    {
        if (digits)
        {
            continue;
        }
        bool acronym = letters >= 2 && letters <= 4 && capitals == letters;
        if ((letters < 4 && !acronym) || stop_word(folded))
        {
            continue;
        }
        uint32_t h = stem_hash(folded);
        bool seen = false;
        for (int i = 0; i < count; i++)
        {
            seen |= out[i].hash == h;
        }
        if (seen)
        {
            continue;
        }
        out[count].hash = h;
        nw_copy(out[count].word, sizeof(out[count].word), word);
        count++;
    }
    return count;
}

void nw_fold(const char *in, char *out, size_t size)
{
    const unsigned char *u = (const unsigned char *)in;
    size_t n = 0;
    bool space = true;
    while (*u && n + 1 < size)
    {
        int len;
        bool upper;
        char c = fold(u, &len, &upper);
        u += len;
        if (c == 0)
        {
            if (!space)
            {
                out[n++] = ' ';
                space = true;
            }
            continue;
        }
        out[n++] = c;
        space = false;
    }
    out[n] = '\0';
}

int nw_needles(const char *query, char needles[][8], int max)
{
    char folded[160];
    nw_fold(query, folded, sizeof(folded));
    int count = 0;
    char *p = folded;
    while (*p && count < max)
    {
        while (*p == ' ')
        {
            p++;
        }
        char *start = p;
        while (*p && *p != ' ')
        {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len == 0)
        {
            break;
        }
        char word[40];
        len = len < sizeof(word) - 1 ? len : sizeof(word) - 1;
        memcpy(word, start, len);
        word[len] = '\0';
        if ((len < 3 && count > 0) || stop_word(word) || (len < 2))
        {
            continue;
        }
        size_t keep = len > 6 ? 6 : len;
        memcpy(needles[count], word, keep);
        needles[count][keep] = '\0';
        count++;
    }
    return count;
}

bool nw_match(const char *folded_title, char needles[][8], int count)
{
    for (int i = 0; i < count; i++)
    {
        const char *p = folded_title;
        bool found = false;
        size_t n = strlen(needles[i]);
        while ((p = strstr(p, needles[i])) != NULL)
        {
            if (p == folded_title || p[-1] == ' ')
            {
                found = true;
                break;
            }
            p += n;
        }
        if (!found)
        {
            return false;
        }
    }
    return count > 0;
}

uint32_t nw_stem_hash(const char *word)
{
    char folded[40], original[NW_WORD];
    int capitals, letters;
    bool digits;
    if (next_word(word, folded, sizeof(folded), original, sizeof(original), &capitals, &letters, &digits) == NULL ||
        stop_word(folded))
    {
        return 0;
    }
    return stem_hash(folded);
}

/* ------------------------------------------------------------------ dates */

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static int month_of(const char *p)
{
    static const char *const en[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    static const char *const it[] = {"gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic"};
    char m[4] = {(char)tolower((unsigned char)p[0]), (char)tolower((unsigned char)p[1]), (char)tolower((unsigned char)p[2]), 0};
    for (int i = 0; i < 12; i++)
    {
        if (strcmp(m, en[i]) == 0 || strcmp(m, it[i]) == 0)
        {
            return i + 1;
        }
    }
    return 0;
}

static int zone_minutes(const char *p)
{
    while (*p == ' ')
    {
        p++;
    }
    if (*p == '+' || *p == '-')
    {
        int sign = *p == '-' ? -1 : 1;
        p++;
        int hh = 0, mm = 0;
        if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]))
        {
            hh = (p[0] - '0') * 10 + (p[1] - '0');
            p += 2;
            if (*p == ':')
            {
                p++;
            }
            if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]))
            {
                mm = (p[0] - '0') * 10 + (p[1] - '0');
            }
        }
        return sign * (hh * 60 + mm);
    }
    if (strncmp(p, "CEST", 4) == 0) return 120;
    if (strncmp(p, "CET", 3) == 0) return 60;
    if (strncmp(p, "EDT", 3) == 0) return -240;
    if (strncmp(p, "EST", 3) == 0) return -300;
    if (strncmp(p, "PDT", 3) == 0) return -420;
    if (strncmp(p, "PST", 3) == 0) return -480;
    return 0;   /* GMT, UT, UTC, Z or nothing */
}

/* Up to `max` digits; -1 if none. */
static int read_int(const char **p, int max)
{
    int value = 0, n = 0;
    while (n < max && isdigit((unsigned char)**p))
    {
        value = value * 10 + (**p - '0');
        (*p)++;
        n++;
    }
    return n ? value : -1;
}

static bool expect(const char **p, char c)
{
    if (**p != c)
    {
        return false;
    }
    (*p)++;
    return true;
}

static void skip_spaces(const char **p)
{
    while (**p == ' ')
    {
        (*p)++;
    }
}

int64_t nw_date(const char *text)
{
    const char *p = text;
    skip_spaces(&p);
    int y, mo, d, h = 0, mi = 0, s = 0;
    int64_t zone;
    if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) &&
        isdigit((unsigned char)p[3]) && p[4] == '-')
    {
        /* ISO 8601: 2026-10-14T16:05:00+02:00 */
        y = read_int(&p, 4);
        if (!expect(&p, '-') || (mo = read_int(&p, 2)) < 0 || !expect(&p, '-') || (d = read_int(&p, 2)) < 0)
        {
            return 0;
        }
        if (*p == 'T' || *p == ' ')
        {
            p++;
            if ((h = read_int(&p, 2)) < 0 || !expect(&p, ':') || (mi = read_int(&p, 2)) < 0)
            {
                return 0;
            }
            if (expect(&p, ':'))
            {
                s = read_int(&p, 2);
                s = s < 0 ? 0 : s;
            }
            while (*p == '.' || isdigit((unsigned char)*p))
            {
                p++;
            }
        }
        zone = zone_minutes(p);
    }
    else
    {
        /* RFC 822: "Tue, 14 Oct 2026 16:05:00 +0200" (the day name is optional) */
        const char *comma = strchr(p, ',');
        if (comma != NULL && comma - p < 12)
        {
            p = comma + 1;
        }
        skip_spaces(&p);
        if ((d = read_int(&p, 2)) < 0)
        {
            return 0;
        }
        skip_spaces(&p);
        mo = month_of(p);
        while (*p && *p != ' ')
        {
            p++;
        }
        skip_spaces(&p);
        if (mo == 0 || (y = read_int(&p, 4)) < 0)
        {
            return 0;
        }
        skip_spaces(&p);
        if ((h = read_int(&p, 2)) < 0 || !expect(&p, ':') || (mi = read_int(&p, 2)) < 0)
        {
            return 0;
        }
        if (expect(&p, ':'))
        {
            s = read_int(&p, 2);
            s = s < 0 ? 0 : s;
        }
        if (y < 100)
        {
            y += 2000;
        }
        zone = zone_minutes(p);
    }
    if (y < 1990 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60)
    {
        return 0;
    }
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s - zone * 60;
}
