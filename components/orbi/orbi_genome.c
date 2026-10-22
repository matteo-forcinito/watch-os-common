/* Orbi genome, see orbi/orbi_genome.h and docs/ORBI-GENOME.md. Draw order is part of the format. */
#include "orbi/orbi_genome.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *const SYLLABLES[40] = {
    "lu", "mi", "ko", "na", "ri", "te", "vo", "sa", "ni", "po", "ra", "zu", "be", "di", "ke", "la", "mo", "nu", "pi", "ru",
    "si", "ta", "ve", "yo", "fi", "go", "ma", "ne", "ro", "tu", "bi", "da", "ki", "le", "mu", "no", "pa", "re", "so", "ti",
};
static const char *const ENDINGS[6] = {"", "", "", "n", "l", "x"};

typedef struct
{
    uint64_t state;
} splitmix_t;

static uint64_t next(splitmix_t *g)
{
    g->state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = g->state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint32_t pick(splitmix_t *g, uint32_t n)
{
    return (uint32_t)(next(g) % n);
}

uint32_t orbi_fnv1a32(const uint8_t *data, int len)
{
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < len; i++)
    {
        h = (h ^ data[i]) * 0x01000193u;
    }
    return h;
}

uint64_t orbi_seed(uint8_t caps, const uint8_t mac[6], const char *model, uint8_t index, uint32_t birth)
{
    uint8_t buf[6 + 32];
    size_t n = strlen(model);
    n = n > 32 ? 32 : n;
    memcpy(buf, mac, 6);
    memcpy(buf + 6, model, n);
    uint32_t device = orbi_fnv1a32(buf, (int)(6 + n)) & 0xFFFFFFu;
    uint8_t ind[5] = {index, (uint8_t)birth, (uint8_t)(birth >> 8), (uint8_t)(birth >> 16), (uint8_t)(birth >> 24)};
    uint32_t individual = orbi_fnv1a32(ind, 5);
    return ((uint64_t)caps << 56) | ((uint64_t)device << 32) | individual;
}

void orbi_genome(uint64_t seed, orbi_genome_t *t)
{
    memset(t, 0, sizeof(*t));
    t->seed = seed;
    uint8_t caps = (uint8_t)(seed >> 56);
    t->caps = caps;

    splitmix_t fam = {seed >> 32};
    t->hue = (uint16_t)pick(&fam, 360);
    t->body = (caps & ORBI_CAP_SQUARE) ? ORBI_BODY_CUBE : (caps & ORBI_CAP_ROUND) ? ORBI_BODY_DROP : ORBI_BODY_CAPSULE;
    t->stretch = (uint8_t)(90 + pick(&fam, 21));
    if (caps & ORBI_CAP_CAMERA)
    {
        t->ear = ORBI_EAR_NONE;
    }
    else if (caps & ORBI_CAP_MIC)
    {
        t->ear = pick(&fam, 2) ? ORBI_EAR_ROUND : ORBI_EAR_POINTY;
    }
    else
    {
        t->ear = pick(&fam, 3) == 0 ? ORBI_EAR_NUBS : ORBI_EAR_NONE;
    }
    if (caps & ORBI_CAP_KNOB)
    {
        t->crest = ORBI_CREST_ANTENNA;
    }
    else if (caps & ORBI_CAP_SPEAKER)
    {
        t->crest = pick(&fam, 2) ? ORBI_CREST_NOTE : ORBI_CREST_SPROUT;
    }
    else
    {
        static const uint8_t crests[3] = {ORBI_CREST_NONE, ORBI_CREST_SPROUT, ORBI_CREST_HORN};
        t->crest = crests[pick(&fam, 3)];
    }

    splitmix_t ind = {seed};
    t->hue2 = (uint16_t)((t->hue + 300 + pick(&ind, 121)) % 360);
    t->eyes = (caps & ORBI_CAP_CAMERA) ? ORBI_EYES_CYCLOPS : (uint8_t)pick(&ind, 5);
    t->eye_gap = (uint8_t)(34 + pick(&ind, 15));
    t->eye_size = (uint8_t)(14 + pick(&ind, 9));
    t->pattern = (uint8_t)pick(&ind, 5);
    t->cheeks = pick(&ind, 3) != 0;
    t->curiosity = (uint8_t)(20 + pick(&ind, 81));
    t->energy = (uint8_t)(20 + pick(&ind, 81));
    t->sociability = (uint8_t)(20 + pick(&ind, 81));
    t->appetite = (uint8_t)(20 + pick(&ind, 81));
    t->playfulness = (uint8_t)(20 + pick(&ind, 81));
    int count = 2 + (pick(&ind, 4) == 0 ? 1 : 0);
    for (int i = 0; i < count; i++)
    {
        strcat(t->name, SYLLABLES[pick(&ind, 40)]);
    }
    strcat(t->name, ENDINGS[pick(&ind, 6)]);
    t->name[0] = (char)toupper((unsigned char)t->name[0]);
}

void orbi_code(uint64_t seed, char out[17])
{
    snprintf(out, 17, "%08lx%08lx", (unsigned long)(seed >> 32), (unsigned long)(seed & 0xFFFFFFFFu));
}

bool orbi_parse(const char *code, uint64_t *seed)
{
    uint64_t v = 0;
    int n = 0;
    for (; code[n] != 0; n++)
    {
        int c = tolower((unsigned char)code[n]);
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0 || n >= 16)
        {
            return false;
        }
        v = (v << 4) | (uint64_t)d;
    }
    if (n != 16)
    {
        return false;
    }
    *seed = v;
    return true;
}
