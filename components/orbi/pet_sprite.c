/* Pixel-art animals, see orbi/pet_sprite.h and docs/ORBI-PETS.md.
 * Geometry is integer, in quarter pixels (Q4): ports must give the same grid. */
#include "orbi/pet_sprite.h"
#include "orbi/pet_pokemon.h"

#include <string.h>

#define P(v) ((int)((v) * 4))
#define GROUND P(31)

typedef struct
{
    uint64_t state;
} rng_t;

static uint64_t next(rng_t *r)
{
    r->state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = r->state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint32_t pick(rng_t *r, uint32_t n)
{
    return (uint32_t)(next(r) % n);
}

/* ------------------------------------------------------------- catalogue */

typedef struct
{
    const char *name;
    uint32_t body, belly, mark, accent, accent2;
} variant_t;

static const variant_t DOG[] = {
    {"Biscotto", 0xC98B4F, 0xF2D7B0, 0x7A4A26, 0x2A1A14, 0x2A1A14},
    {"Miele", 0xE3B062, 0xFAE6C0, 0xB07A34, 0x3A2616, 0x3A2616},
    {"Carbone", 0x3B3A40, 0xBFB8B0, 0x1E1D22, 0x111014, 0x111014},
    {"Neve", 0xEDEBE6, 0xFFFFFF, 0xB9A58E, 0x2A2420, 0x2A2420},
    {"Nebbia", 0x9AA0A8, 0xD9DDE2, 0x5F656E, 0x22252A, 0x22252A},
    {"Cioccolato", 0x6B3F2A, 0xC9906A, 0x3E2216, 0x1C100A, 0x1C100A},
};
static const variant_t CAT[] = {
    {"Rosso", 0xE08A3C, 0xF7D4A8, 0xB0581C, 0xFF8FA3, 0x2A1A14},
    {"Grigio", 0x8D939B, 0xD6DADF, 0x5C626A, 0xFF9FB0, 0x2A2C30},
    {"Notte", 0x2F2E36, 0x6E6A78, 0x17161C, 0xD97A8C, 0x111014},
    {"Latte", 0xF1ECE4, 0xFFFFFF, 0xC8B9A6, 0xFF8FA3, 0x6B5A4A},
    {"Crema", 0xE9CFA2, 0xFBF0DC, 0xC9A26A, 0xFF8FA3, 0x6B4A2A},
    {"Fumo", 0x6F6A80, 0xB9B4C8, 0x45415A, 0xE58FA8, 0x2A2834},
};
static const variant_t FISH[] = {
    {"Oro", 0xF39A2B, 0xFFD27A, 0xD9601A, 0xFFFFFF, 0xFFB347},
    {"Mare", 0x3C8DE0, 0x9FD4FF, 0x1F5BB0, 0xFFFFFF, 0x63C5FF},
    {"Corallo", 0xFF6F61, 0xFFC2B5, 0xC9403A, 0xFFFFFF, 0xFF9E8A},
    {"Limone", 0xF4D23C, 0xFFF3A8, 0xC9A01E, 0xFFFFFF, 0xFFE57A},
    {"Ametista", 0x9B6BE0, 0xD7C2FF, 0x6A3FB0, 0xFFFFFF, 0xC6A2FF},
    {"Neon", 0x2FD3C8, 0xA8FFF6, 0x1A8F88, 0xFFFFFF, 0xFF5FA2},
};
static const variant_t CHAMELEON[] = {
    {"Foglia", 0x5BBF4A, 0xC5EE8A, 0x2E7D32, 0xF2C14E, 0x3E9E3A},
    {"Laguna", 0x2EB8A8, 0xA6F0E4, 0x16756C, 0xF2C14E, 0x1F8F84},
    {"Tramonto", 0xF08A3C, 0xFFD0A0, 0xB0501C, 0xFFE07A, 0xD0602A},
    {"Lampone", 0xE0507A, 0xFFB8CC, 0x9E2A50, 0xFFE07A, 0xB83A62},
    {"Cielo", 0x4FA3E8, 0xBFE2FF, 0x2A66A8, 0xFFE07A, 0x3A84C8},
};
static const variant_t GECKO[] = {
    {"Leopardo", 0xF2C45A, 0xFFF0C8, 0x5A3E2B, 0xE89A6A, 0xD9A23A},
    {"Smeraldo", 0x3DBF72, 0xC8F5D8, 0x1E7A44, 0x9BE8B8, 0x2A9A5A},
    {"Papaya", 0xF58B4C, 0xFFD6B8, 0xB04E1E, 0xFFB08A, 0xD86A30},
    {"Rosa", 0xF2A0B8, 0xFFE0EA, 0xC2185B, 0xFF7FA8, 0xE07A98},
    {"Notte", 0x5A5F7A, 0xB8BCD8, 0x2E3148, 0x9AA0C8, 0x44486A},
};
static const variant_t HEN[] = {
    {"Bianca", 0xF3F0E8, 0xFFFFFF, 0xC9C2B4, 0xF2A93B, 0xE53935},
    {"Rossa", 0xB5562A, 0xE29A5B, 0x6E2E14, 0xF2A93B, 0xE53935},
    {"Nera", 0x2C2C34, 0x55556A, 0x14141A, 0xF2A93B, 0xE53935},
    {"Dorata", 0xE0A83C, 0xF5D58A, 0xA86A1E, 0xF2A93B, 0xE53935},
    {"Grigia", 0x9FA4AA, 0xDDE0E4, 0x5F646A, 0xF2A93B, 0xE53935},
};
static const variant_t GOAT[] = {
    {"Bianca", 0xF0EDE6, 0xFFFFFF, 0xC8BBA8, 0x5A4A42, 0xB5A58C},
    {"Bruna", 0x8A5A3A, 0xD2A27A, 0x4E2E1A, 0x2A1A12, 0xC9B79A},
    {"Nera", 0x34323A, 0x6A6672, 0x18171C, 0x101014, 0xA89A86},
    {"Camoscio", 0xB7834E, 0xE6CBA6, 0x3A2A20, 0x2A1A12, 0x8A7A66},
    {"Grigia", 0xA2A0A0, 0xE0DEDC, 0x66625E, 0x2A2826, 0xC2B8A8},
};
static const variant_t BUNNY[] = {
    {"Neve", 0xF4F2EE, 0xFFFFFF, 0xCFC6BA, 0xFF8FA3, 0xFF8FA3},
    {"Nocciola", 0xB98A60, 0xEBD2B4, 0x7A5638, 0xFF8FA3, 0xE8A0A8},
    {"Grigio", 0x9A9AA2, 0xDCDCE2, 0x64646C, 0xFF8FA3, 0xE8A0A8},
    {"Nero", 0x3A3940, 0x7A7884, 0x1C1B20, 0xE07A8C, 0xC07080},
    {"Cannella", 0xD08B4C, 0xF6D6B0, 0x8E5428, 0xFF8FA3, 0xE8A0A8},
};

typedef struct
{
    const char *name;
    const variant_t *variants;
    uint8_t count;
    uint8_t marks[4];
    uint8_t mark_count;
} species_t;

#define N(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))
static const species_t SPECIES[PET_SPECIES] = {
    {"Cane", DOG, N(DOG), {PET_MARK_NONE, PET_MARK_SPOTS, PET_MARK_SOCKS, PET_MARK_PATCH}, 4},
    {"Gatto", CAT, N(CAT), {PET_MARK_NONE, PET_MARK_STRIPES, PET_MARK_SOCKS, PET_MARK_PATCH}, 4},
    {"Pesce", FISH, N(FISH), {PET_MARK_NONE, PET_MARK_STRIPES, PET_MARK_SPOTS}, 3},
    {"Camaleonte", CHAMELEON, N(CHAMELEON), {PET_MARK_NONE, PET_MARK_STRIPES, PET_MARK_SPOTS}, 3},
    {"Geco", GECKO, N(GECKO), {PET_MARK_SPOTS, PET_MARK_STRIPES, PET_MARK_NONE}, 3},
    {"Gallina", HEN, N(HEN), {PET_MARK_NONE, PET_MARK_SPOTS}, 2},
    {"Capra", GOAT, N(GOAT), {PET_MARK_NONE, PET_MARK_PATCH, PET_MARK_SOCKS}, 3},
    {"Coniglio", BUNNY, N(BUNNY), {PET_MARK_NONE, PET_MARK_PATCH, PET_MARK_SOCKS}, 3},
};

int pet_level(int32_t xp)
{
    if (xp < 0)
        xp = 0;
    int32_t q = xp / 40, r = 0;
    while ((r + 1) * (r + 1) <= q)
        r++;
    return 1 + r;
}

void pet_look(uint64_t seed, int level, pet_look_t *out)
{
    memset(out, 0, sizeof(*out));
    rng_t r = {seed ^ 0x5045545350454331ULL};   /* "PETSPEC1" */
    out->species = (uint8_t)pick(&r, PET_SPECIES);
    const species_t *s = &SPECIES[out->species];
    out->variant = (uint8_t)pick(&r, s->count);
    out->mark = s->marks[pick(&r, s->mark_count)];
    out->cheeks = pick(&r, 3) != 0;
    if (level < 1)
        level = 1;
    out->level = (uint8_t)(level > 255 ? 255 : level);
    int g = (level - 1) * 255 / 14;
    out->growth = (uint8_t)(g > 255 ? 255 : g);
    out->stage = level < 5 ? 0 : level < 10 ? 1 : level < 20 ? 2 : 3;
#ifdef WATCH_PET_POKEMON
    pet_pokemon_get(seed, &out->pokemon_dex, &out->pokemon_shiny);
#endif
}

uint16_t pet_breed_key(const pet_look_t *l)
{
    return (uint16_t)(l->species << 8 | l->variant << 4 | l->mark);
}

const char *pet_species_name(int species)
{
    return species >= 0 && species < PET_SPECIES ? SPECIES[species].name : "?";
}

const char *pet_look_name(const pet_look_t *l)
{
#ifdef WATCH_PET_POKEMON
    if (l->pokemon_dex) return pet_pokemon_name(l->pokemon_dex);
#endif
    return pet_species_name(l->species);
}

const char *pet_variant_name(const pet_look_t *l)
{
#ifdef WATCH_PET_POKEMON
    if (l->pokemon_dex) return l->pokemon_shiny ? "Shiny" : "";
#endif
    const species_t *s = &SPECIES[l->species % PET_SPECIES];
    return s->variants[l->variant % s->count].name;
}

const char *pet_mark_name(int mark)
{
    static const char *const NAMES[PET_MARKS] = {"", "a macchie", "tigrato", "con i calzini", "pezzato"};
    return mark >= 0 && mark < PET_MARKS ? NAMES[mark] : "";
}

const char *pet_stage_name(int stage)
{
    static const char *const NAMES[4] = {"Cucciolo", "Giovane", "Adulto", "Saggio"};
    return NAMES[stage & 3];
}

int pet_variant_count(int species)
{
    return species >= 0 && species < PET_SPECIES ? SPECIES[species].count : 0;
}

/* --------------------------------------------------------------- palette */

static uint32_t scale(uint32_t c, int pct)
{
    int r = ((c >> 16) & 0xFF) * pct / 100, g = ((c >> 8) & 0xFF) * pct / 100, b = (c & 0xFF) * pct / 100;
    r = r > 255 ? 255 : r;
    g = g > 255 ? 255 : g;
    b = b > 255 ? 255 : b;
    return (uint32_t)(r << 16 | g << 8 | b);
}

static uint32_t mix(uint32_t a, uint32_t b, int pct_b)
{
    int r = (((a >> 16) & 0xFF) * (100 - pct_b) + ((b >> 16) & 0xFF) * pct_b) / 100;
    int g = (((a >> 8) & 0xFF) * (100 - pct_b) + ((b >> 8) & 0xFF) * pct_b) / 100;
    int bl = ((a & 0xFF) * (100 - pct_b) + (b & 0xFF) * pct_b) / 100;
    return (uint32_t)(r << 16 | g << 8 | bl);
}

void pet_palette(const pet_look_t *l, int mood, uint32_t out[PET_COLORS])
{
    memset(out, 0, PET_COLORS * sizeof(*out));
#ifdef WATCH_PET_POKEMON
    if (l->pokemon_dex && pet_pokemon_palette(l->pokemon_dex, l->pokemon_shiny, out)) return;
#endif
    const species_t *s = &SPECIES[l->species % PET_SPECIES];
    variant_t v = s->variants[l->variant % s->count];
    if (l->species == PET_HEN && l->growth < 90)
    {
        /* A chick is yellow whatever hen it becomes. */
        v.body = 0xFFD54A;
        v.belly = 0xFFF0A0;
        v.mark = 0xE8B020;
    }
    if (l->species == PET_CHAMELEON)
    {
        if (mood == 1)
            v.body = mix(v.body, v.accent, 45);
        else if (mood == 2)
            v.body = mix(v.body, 0x8A8F96, 55);
        else if (mood == 3)
            v.body = scale(v.body, 70);
    }
    out[PET_C_CLEAR] = 0;
    out[PET_C_OUTLINE] = 0xFF000000u | mix(scale(v.body, 30), 0x141826, 45);
    out[PET_C_BODY] = 0xFF000000u | v.body;
    out[PET_C_SHADE] = 0xFF000000u | scale(v.body, 78);
    out[PET_C_BELLY] = 0xFF000000u | v.belly;
    out[PET_C_MARK] = 0xFF000000u | v.mark;
    out[PET_C_EYE] = 0xFF15121Eu;
    out[PET_C_SHINE] = 0xFFFFFFFFu;
    out[PET_C_ACCENT] = 0xFF000000u | v.accent;
    out[PET_C_CHEEK] = 0xFFFF8FA3u;
    out[PET_C_GLASS] = 0x605AB4FFu;
    out[PET_C_ACCENT2] = 0xFF000000u | v.accent2;
}

/* --------------------------------------------------------------- drawing */

typedef struct
{
    const pet_look_t *l;
    uint8_t *G;
    int g;          /* growth 0..255 */
    int oy;         /* whole-sprite vertical offset, pixels */
    int frame;
    pet_pose_t pose;
    int eyes;       /* 0 open, 1 closed, 2 happy, 3 sad */
} ctx_t;

static int lerp(int a, int b, int g)
{
    return a + (b - a) * g / 255;
}

static void put(ctx_t *c, int x, int y, uint8_t col)
{
    y += c->oy;
    if ((unsigned)x < PET_GRID && (unsigned)y < PET_GRID)
        c->G[y * PET_GRID + x] = col;
}

static uint8_t get(ctx_t *c, int x, int y)
{
    y += c->oy;
    return (unsigned)x < PET_GRID && (unsigned)y < PET_GRID ? c->G[y * PET_GRID + x] : PET_C_CLEAR;
}

static bool body_pixel(uint8_t v)
{
    return v == PET_C_BODY || v == PET_C_SHADE;
}

/* Filled ellipse, centre and radii in Q4. shade: lower band colour (0 none).
 * mask: only recolour body pixels (bellies, patches). */
static void ellipse_ex(ctx_t *c, int cx, int cy, int rx, int ry, uint8_t col, uint8_t shade, bool mask)
{
    if (rx < 2 || ry < 2)
        return;
    int64_t rx2 = (int64_t)rx * rx, ry2 = (int64_t)ry * ry, lim = rx2 * ry2;
    for (int y = (cy - ry) / 4 - 1; y <= (cy + ry) / 4 + 1; y++)
        for (int x = (cx - rx) / 4 - 1; x <= (cx + rx) / 4 + 1; x++)
        {
            int64_t dx = x * 4 + 2 - cx, dy = y * 4 + 2 - cy;
            if (dx * dx * ry2 + dy * dy * rx2 > lim)
                continue;
            if (mask && !body_pixel(get(c, x, y)))
                continue;
            put(c, x, y, shade && dy * 100 > (int64_t)ry * 38 ? shade : col);
        }
}

static void ellipse(ctx_t *c, int cx, int cy, int rx, int ry, uint8_t col, uint8_t shade)
{
    ellipse_ex(c, cx, cy, rx, ry, col, shade, false);
}

/* Thick segment (capsule) in Q4, width w. */
static void seg(ctx_t *c, int x0, int y0, int x1, int y1, int w, uint8_t col)
{
    int minx = (x0 < x1 ? x0 : x1) - w, maxx = (x0 > x1 ? x0 : x1) + w;
    int miny = (y0 < y1 ? y0 : y1) - w, maxy = (y0 > y1 ? y0 : y1) + w;
    int64_t vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    int64_t r2 = (int64_t)w * w / 4;
    for (int y = miny / 4 - 1; y <= maxy / 4 + 1; y++)
        for (int x = minx / 4 - 1; x <= maxx / 4 + 1; x++)
        {
            int64_t px = x * 4 + 2 - x0, py = y * 4 + 2 - y0, t = len2 ? px * vx + py * vy : 0;
            int64_t dx, dy;
            if (t <= 0 || !len2)
                dx = px, dy = py;
            else if (t >= len2)
                dx = x * 4 + 2 - x1, dy = y * 4 + 2 - y1;
            else
            {
                /* distance to the line: |cross| / len */
                int64_t cr = px * vy - py * vx;
                if (cr * cr <= r2 * len2)
                    put(c, x, y, col);
                continue;
            }
            if (dx * dx + dy * dy <= r2)
                put(c, x, y, col);
        }
}

static int64_t edge(int ax, int ay, int bx, int by, int px, int py)
{
    return (int64_t)(bx - ax) * (py - ay) - (int64_t)(by - ay) * (px - ax);
}

static void tri(ctx_t *c, int ax, int ay, int bx, int by, int cx, int cy, uint8_t col)
{
    int minx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    int maxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    int miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    int maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    for (int y = miny / 4 - 1; y <= maxy / 4 + 1; y++)
        for (int x = minx / 4 - 1; x <= maxx / 4 + 1; x++)
        {
            int px = x * 4 + 2, py = y * 4 + 2;
            int64_t e0 = edge(ax, ay, bx, by, px, py), e1 = edge(bx, by, cx, cy, px, py), e2 = edge(cx, cy, ax, ay, px, py);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))
                put(c, x, y, col);
        }
}

/* Eye at pixel x, y; big = 2x2 (cubs and large heads). */
static void eye(ctx_t *c, int x, int y, bool big)
{
    switch (c->eyes)
    {
    case 1:   /* closed */
        put(c, x, y + 1, PET_C_OUTLINE);
        if (big)
            put(c, x + 1, y + 1, PET_C_OUTLINE);
        break;
    case 2:   /* happy ^ */
        put(c, x, y + 1, PET_C_OUTLINE);
        put(c, x + 1, y, PET_C_OUTLINE);
        if (big)
            put(c, x + 2, y + 1, PET_C_OUTLINE);
        break;
    case 3:   /* sad: low and heavy */
        put(c, x, y + 1, PET_C_EYE);
        if (big)
            put(c, x + 1, y + 1, PET_C_EYE);
        put(c, x - 1, y, PET_C_OUTLINE);
        break;
    default:
        put(c, x, y, big ? PET_C_SHINE : PET_C_EYE);
        put(c, x, y + 1, PET_C_EYE);
        if (big)
        {
            put(c, x + 1, y, PET_C_EYE);
            put(c, x + 1, y + 1, PET_C_EYE);
        }
        break;
    }
}

/* Marks recolour body pixels inside a box (pixels). */
static void marks(ctx_t *c, int x0, int y0, int x1, int y1, uint64_t salt)
{
    switch (c->l->mark)
    {
    case PET_MARK_SPOTS:
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
            {
                uint32_t h = (uint32_t)((x / 2) * 73856093u ^ (y / 2) * 19349663u ^ (uint32_t)salt);
                h ^= h >> 13;
                h *= 0x5bd1e995u;
                h ^= h >> 15;
                if (h % 6 == 0 && ((x + y) & 1) == 0 && body_pixel(get(c, x, y)))
                    put(c, x, y, PET_C_MARK);
            }
        break;
    case PET_MARK_STRIPES:
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                if (((x + y / 3) % 4) == 0 && body_pixel(get(c, x, y)))
                    put(c, x, y, PET_C_MARK);
        break;
    default:
        break;
    }
}

/* Lower pose offsets shared by the animals. */
static int leg_swing(ctx_t *c, int which)
{
    if (c->pose != PET_POSE_WALK)
        return 0;
    static const int8_t PHASE[4] = {1, 0, -1, 0};
    return PHASE[(c->frame + which * 2) & 3] * P(1);
}

static int breath(ctx_t *c)
{
    if (c->pose == PET_POSE_IDLE)
        return ((c->frame >> 1) & 1) ? 1 : 0;
    if (c->pose == PET_POSE_WALK)
        return c->frame & 1;
    return 0;
}

/* ---------------------------------------------------------- quadrupeds */

typedef struct
{
    int brx0, brx1, bry0, bry1;    /* body radii, cub -> adult */
    int leg0, leg1, legw;          /* leg length, width */
    int hr0, hr1;                  /* head radius */
    int hdx0, hdx1, hdy0, hdy1;    /* head centre from the body centre */
} quad_t;

static void quadruped(ctx_t *c, const quad_t *q)
{
    int g = c->g;
    int brx = lerp(q->brx0, q->brx1, g), bry = lerp(q->bry0, q->bry1, g);
    int leg = lerp(q->leg0, q->leg1, g), hr = lerp(q->hr0, q->hr1, g);
    bool lying = c->pose == PET_POSE_SLEEP;
    if (lying)
        leg = P(0.5);
    int cx = P(13.5), cy = GROUND - leg - bry * 6 / 10 + breath(c) * P(0.5);
    int hx = cx + lerp(q->hdx0, q->hdx1, g), hy = cy + lerp(q->hdy0, q->hdy1, g);
    if (c->pose == PET_POSE_EAT)
        hx += P(1), hy += P(3) + (c->frame & 1) * P(1);
    else if (c->pose == PET_POSE_SAD || lying)
        hy += P(1.5);
    int sp = c->l->species;

    /* Tail first (behind the body). */
    int tx = cx - brx * 85 / 100, ty = cy - bry * 3 / 10;
    int tl = lerp(P(2), P(5), g);
    int wag = (c->pose == PET_POSE_HAPPY || c->pose == PET_POSE_WALK || c->pose == PET_POSE_IDLE) ? ((c->frame & 1) ? P(1) : -P(1)) : 0;
    if (sp == PET_DOG)
    {
        int up = c->pose == PET_POSE_SAD || lying ? P(1) : -tl;
        seg(c, tx, ty, tx - tl * 6 / 10 + wag, ty + up, P(1.8), PET_C_BODY);
    }
    else if (sp == PET_CAT)
    {
        int mx = tx - tl * 7 / 10, my = ty - tl * 6 / 10;
        if (lying)
            seg(c, tx, ty, tx + P(2), GROUND - P(1), P(1.5), PET_C_BODY);
        else
        {
            seg(c, tx, ty, mx, my, P(1.6), PET_C_BODY);
            seg(c, mx, my, mx + wag / 2 + P(0.5), my - tl * 7 / 10, P(1.5), PET_C_BODY);
            put(c, (mx + wag / 2 + P(0.5)) / 4, (my - tl * 7 / 10) / 4, PET_C_MARK);
        }
    }
    else if (sp == PET_GOAT)
        seg(c, tx, ty, tx - P(1.2), ty - P(2) - (wag > 0 ? P(0.5) : 0), P(1.6), PET_C_BODY);

    /* Far legs (shaded), body, near legs. */
    int lx[2] = {cx - brx * 55 / 100, cx + brx * 50 / 100};
    int top = cy + bry / 3;
    for (int i = 0; i < 2 && !lying; i++)
        seg(c, lx[i] + P(1.5), top, lx[i] + P(1.5) - leg_swing(c, i + 1), GROUND - q->legw / 2, q->legw, PET_C_SHADE);
    ellipse(c, cx, cy, brx, bry, PET_C_BODY, PET_C_SHADE);
    ellipse_ex(c, cx + brx * 2 / 10, cy + bry * 45 / 100, brx * 6 / 10, bry * 45 / 100, PET_C_BELLY, 0, true);
    if (c->l->mark == PET_MARK_PATCH)
        ellipse_ex(c, cx - brx * 25 / 100, cy - bry * 3 / 10, brx * 45 / 100, bry * 55 / 100, PET_C_MARK, 0, true);
    marks(c, (cx - brx) / 4, (cy - bry) / 4, (cx + brx) / 4, (cy + bry) / 4, c->l->variant);
    for (int i = 0; i < 2 && !lying; i++)
    {
        int x = lx[i] + leg_swing(c, i);
        seg(c, lx[i], top, x, GROUND - q->legw / 2, q->legw, PET_C_BODY);
        if (c->l->mark == PET_MARK_SOCKS)
            seg(c, x, GROUND - leg * 35 / 100, x, GROUND - q->legw / 2, q->legw, PET_C_BELLY);
        if (sp == PET_GOAT)
            seg(c, x, GROUND - P(0.8), x, GROUND - P(0.6), q->legw, PET_C_ACCENT2);
    }

    /* Ears behind the head, head, face. */
    if (sp == PET_CAT)
    {
        tri(c, hx - hr * 7 / 10, hy - hr * 4 / 10, hx - hr * 1 / 10, hy - hr * 8 / 10, hx - hr * 6 / 10, hy - hr * 16 / 10, PET_C_SHADE);
        tri(c, hx - hr * 1 / 10, hy - hr * 8 / 10, hx + hr * 6 / 10, hy - hr * 5 / 10, hx + hr * 3 / 10, hy - hr * 15 / 10, PET_C_BODY);
        put(c, (hx + hr * 25 / 100) / 4, (hy - hr * 9 / 10) / 4, PET_C_CHEEK);
    }
    if (sp == PET_GOAT && g > 40)
    {
        int hl = lerp(0, P(5), g);
        int bx = hx - hr * 2 / 10, by = hy - hr * 8 / 10;
        seg(c, bx, by, bx - hl * 5 / 10, by - hl * 6 / 10, P(1.6), PET_C_ACCENT2);
        seg(c, bx - hl * 5 / 10, by - hl * 6 / 10, bx - hl, by - hl * 2 / 10, P(1.3), PET_C_ACCENT2);
    }
    ellipse(c, hx, hy, hr, hr * 92 / 100, PET_C_BODY, PET_C_SHADE);
    if (sp == PET_DOG)
    {
        int sx = hx + hr * 85 / 100, sy = hy + hr * 3 / 10;
        ellipse(c, sx, sy, lerp(P(1.8), P(2.6), g), lerp(P(1.4), P(1.8), g), PET_C_BELLY, 0);
        put(c, (sx + lerp(P(1.4), P(2.2), g)) / 4, (sy - P(0.8)) / 4, PET_C_ACCENT);
        /* Floppy ear. */
        int droop = c->pose == PET_POSE_HAPPY && (c->frame & 1) ? -P(1) : 0;
        ellipse(c, hx - hr * 45 / 100, hy + hr * 1 / 10 + droop, hr * 35 / 100, hr * 7 / 10, PET_C_MARK, 0);
    }
    else if (sp == PET_CAT)
        put(c, (hx + hr * 9 / 10) / 4, (hy + hr * 25 / 100) / 4, PET_C_ACCENT);
    else if (sp == PET_GOAT)
    {
        int sx = hx + hr * 8 / 10, sy = hy + hr * 5 / 10;
        ellipse(c, sx, sy, lerp(P(1.6), P(2.4), g), lerp(P(1.5), P(2), g), PET_C_BODY, PET_C_SHADE);
        put(c, (sx + P(1.6)) / 4, (sy - P(0.4)) / 4, PET_C_ACCENT);
        ellipse(c, hx - hr * 6 / 10, hy - hr * 2 / 10, hr * 45 / 100, hr * 25 / 100, PET_C_SHADE, 0);
        if (g > 130)
            seg(c, sx - P(0.5), sy + P(1.5), sx - P(1), sy + lerp(P(1.5), P(3.5), g), P(1.4), PET_C_BELLY);
    }
    bool big = g < 110;
    int ex = (hx + hr * 35 / 100) / 4, ey = (hy - hr * 3 / 10) / 4;
    eye(c, ex, ey, big);
    if (c->l->cheeks && c->eyes != 1)
        put(c, ex - 1, ey + 2 + (big ? 0 : 0), PET_C_CHEEK);
}

static void dog(ctx_t *c)
{
    static const quad_t Q = {P(5), P(8.5), P(3.3), P(4.6), P(2.5), P(6), P(2.2), P(4.6), P(4.8), P(5), P(8), -P(4.5), -P(5.5)};
    quadruped(c, &Q);
}

static void cat(ctx_t *c)
{
    static const quad_t Q = {P(4.8), P(8), P(3), P(4), P(2.5), P(6), P(1.9), P(4.4), P(4.3), P(4.8), P(7.8), -P(4.2), -P(5.6)};
    quadruped(c, &Q);
}

static void goat(ctx_t *c)
{
    static const quad_t Q = {P(5), P(8), P(3.3), P(4.6), P(3.5), P(7.5), P(2), P(4.2), P(4.2), P(5.2), P(8.4), -P(4.5), -P(6.2)};
    quadruped(c, &Q);
}

/* ------------------------------------------------------------- others */

static void bunny(ctx_t *c)
{
    int g = c->g;
    static const int8_t HOP[4] = {0, -1, -2, -1};
    if (c->pose == PET_POSE_WALK)
        c->oy += HOP[c->frame & 3];
    int rx = lerp(P(4.6), P(6.5), g), ry = lerp(P(4.4), P(6.2), g);
    int cx = P(13), cy = GROUND - ry + breath(c) * P(0.5);
    int hr = lerp(P(4.2), P(4.6), g);
    int hx = cx + rx * 6 / 10, hy = cy - ry * 85 / 100;
    if (c->pose == PET_POSE_EAT)
        hx += P(1), hy += P(2) + (c->frame & 1) * P(0.5);
    bool down = c->pose == PET_POSE_SLEEP || c->pose == PET_POSE_SAD;
    int el = lerp(P(4.5), P(8), g);
    /* Ears: back one shaded, front one with a pink line. */
    int ebx = hx - hr * 3 / 10, eby = hy - hr * 7 / 10;
    if (down)
    {
        seg(c, ebx, eby, ebx - el, eby + P(1), P(2.2), PET_C_SHADE);
        seg(c, ebx + P(1.5), eby, ebx + P(1.5) - el, eby + P(2), P(2.2), PET_C_BODY);
    }
    else
    {
        int sway = c->pose == PET_POSE_HAPPY && (c->frame & 1) ? P(1) : 0;
        seg(c, ebx - P(1), eby, ebx - P(2.5), eby - el, P(2.3), PET_C_SHADE);
        seg(c, ebx + P(1), eby, ebx + P(0.5) + sway, eby - el, P(2.3), PET_C_BODY);
        seg(c, ebx + P(1), eby - P(1.5), ebx + P(0.6) + sway, eby - el + P(1.2), P(0.6), PET_C_CHEEK);
    }
    ellipse(c, cx - rx, cy + ry * 2 / 10, lerp(P(1.6), P(2), g), lerp(P(1.6), P(2), g), PET_C_BELLY, 0);
    ellipse(c, cx, cy, rx, ry, PET_C_BODY, PET_C_SHADE);
    ellipse_ex(c, cx + rx * 35 / 100, cy + ry * 3 / 10, rx * 5 / 10, ry * 6 / 10, PET_C_BELLY, 0, true);
    if (c->l->mark == PET_MARK_PATCH)
        ellipse_ex(c, cx - rx * 3 / 10, cy - ry * 3 / 10, rx * 5 / 10, ry * 45 / 100, PET_C_MARK, 0, true);
    ellipse(c, cx - rx * 3 / 10, GROUND - P(1.6), lerp(P(3), P(4.4), g), P(1.6), c->l->mark == PET_MARK_SOCKS ? PET_C_BELLY : PET_C_SHADE, 0);
    seg(c, cx + rx * 6 / 10, cy + ry * 5 / 10, cx + rx * 7 / 10, GROUND - P(0.8), P(1.8), c->l->mark == PET_MARK_SOCKS ? PET_C_BELLY : PET_C_BODY);
    ellipse(c, hx, hy, hr, hr * 9 / 10, PET_C_BODY, PET_C_SHADE);
    put(c, (hx + hr * 92 / 100) / 4, (hy + hr * 15 / 100) / 4, PET_C_ACCENT);
    int ex = (hx + hr * 3 / 10) / 4, ey = (hy - hr * 3 / 10) / 4;
    eye(c, ex, ey, g < 110);
    if (c->l->cheeks && c->eyes != 1)
        put(c, ex - 1, ey + 2, PET_C_CHEEK);
}

static void hen(ctx_t *c)
{
    int g = c->g;
    int rx = lerp(P(4.4), P(6.6), g), ry = lerp(P(4), P(5.6), g), leg = lerp(P(1.5), P(3.2), g);
    bool lying = c->pose == PET_POSE_SLEEP;
    if (lying)
        leg = 0;
    int cx = P(13.5), cy = GROUND - leg - ry * 9 / 10 + breath(c) * P(0.5);
    int hr = lerp(P(3.2), P(3.3), g);
    int hx = cx + rx * 6 / 10 + (c->pose == PET_POSE_WALK && (c->frame & 1) ? P(1) : 0), hy = cy - ry * 9 / 10;
    if (c->pose == PET_POSE_EAT)
        hx += P(2), hy += P(4) + (c->frame & 1) * P(1.5);
    /* Tail feathers grow with the hen. */
    if (g > 60)
    {
        int tl = lerp(0, P(5), g);
        tri(c, cx - rx * 6 / 10, cy - ry * 2 / 10, cx - rx * 9 / 10, cy + ry * 2 / 10, cx - rx - tl * 4 / 10, cy - ry * 4 / 10 - tl, PET_C_MARK);
    }
    for (int i = 0; i < 2 && !lying; i++)
    {
        int x = cx - P(1.5) + i * P(3) + leg_swing(c, i);
        seg(c, cx - P(1.5) + i * P(3), cy + ry * 7 / 10, x, GROUND - P(0.6), P(1.2), PET_C_ACCENT);
        seg(c, x, GROUND - P(0.6), x + P(1.4), GROUND - P(0.6), P(1.1), PET_C_ACCENT);
    }
    ellipse(c, cx, cy, rx, ry, PET_C_BODY, PET_C_SHADE);
    marks(c, (cx - rx) / 4, (cy - ry) / 4, (cx + rx) / 4, (cy + ry) / 4, 7);
    /* Wing flaps when happy. */
    int flap = c->pose == PET_POSE_HAPPY && (c->frame & 1) ? -P(1.5) : 0;
    ellipse(c, cx - rx * 1 / 10, cy + flap, rx * 55 / 100, ry * 45 / 100, PET_C_SHADE, 0);
    if (g > 80)
    {
        /* Comb: three bumps, wattle under the beak. */
        int cr = lerp(0, P(1.3), g);
        for (int i = 0; i < 3; i++)
            ellipse(c, hx - P(1.2) + i * P(1.3), hy - hr * 9 / 10 - (i == 1 ? P(0.4) : 0), cr, cr, PET_C_ACCENT2, 0);
        if (g > 120)
            ellipse(c, hx + hr * 7 / 10, hy + hr * 8 / 10, P(0.9), lerp(P(0.8), P(1.4), g), PET_C_ACCENT2, 0);
    }
    ellipse(c, hx, hy, hr, hr, PET_C_BODY, 0);
    tri(c, hx + hr * 8 / 10, hy - P(0.8), hx + hr * 8 / 10, hy + P(1), hx + hr + P(2.2), hy + P(0.1), PET_C_ACCENT);
    int ex = (hx + hr * 15 / 100) / 4, ey = (hy - hr * 35 / 100) / 4;
    eye(c, ex, ey, false);
    if (c->l->cheeks && c->eyes != 1)
        put(c, ex - 1, ey + 2, PET_C_CHEEK);
}

static void fish(ctx_t *c)
{
    int g = c->g;
    int br = lerp(P(8), P(11), g);
    int bx = P(15.5), by = GROUND - br;
    /* The water bubble: rim, water, a shine. */
    ellipse(c, bx, by, br, br, PET_C_SHINE, 0);
    ellipse(c, bx, by, br - P(1), br - P(1), PET_C_GLASS, 0);
    for (int i = 0; i < 3; i++)
        put(c, (bx - br * 6 / 10) / 4 + i, (by - br * 6 / 10) / 4 - i / 2 + 1, PET_C_SHINE);
    int swim = c->pose == PET_POSE_WALK || c->pose == PET_POSE_HAPPY ? ((c->frame & 2) ? P(1) : -P(1)) : 0;
    int rx = lerp(P(3.2), P(5.6), g), ry = lerp(P(2.4), P(3.8), g);
    int fx = bx + swim, fy = by + (c->pose == PET_POSE_SLEEP ? P(2) : 0) + (c->pose == PET_POSE_IDLE && (c->frame & 2) ? P(1) : 0);
    int tw = (c->frame & 1) ? P(1) : -P(1);
    if (c->pose == PET_POSE_SLEEP || c->pose == PET_POSE_SAD)
        tw = 0;
    tri(c, fx - rx * 7 / 10, fy, fx - rx - lerp(P(2.5), P(4), g), fy - lerp(P(2.2), P(3.4), g) + tw,
        fx - rx - lerp(P(2.5), P(4), g), fy + lerp(P(2.2), P(3.4), g) + tw, PET_C_ACCENT2);
    tri(c, fx - rx * 4 / 10, fy - ry * 7 / 10, fx + rx * 3 / 10, fy - ry * 8 / 10, fx - rx * 3 / 10, fy - ry - lerp(P(1.5), P(3), g), PET_C_ACCENT2);
    ellipse(c, fx, fy, rx, ry, PET_C_BODY, PET_C_SHADE);
    ellipse_ex(c, fx + rx * 2 / 10, fy + ry * 5 / 10, rx * 6 / 10, ry * 4 / 10, PET_C_BELLY, 0, true);
    marks(c, (fx - rx) / 4, (fy - ry) / 4, (fx + rx) / 4, (fy + ry) / 4, 3);
    tri(c, fx, fy + P(0.5), fx - P(1.5), fy + P(2.5) + tw / 2, fx + P(1), fy + P(2), PET_C_ACCENT2);
    int ex = (fx + rx * 5 / 10) / 4, ey = (fy - ry * 35 / 100) / 4;
    eye(c, ex, ey, g < 110);
    if (c->eyes == 0 && c->pose != PET_POSE_SLEEP && (c->frame & 4))
        put(c, (fx + rx + P(2)) / 4, (fy - ry - P(1)) / 4, PET_C_SHINE);   /* a bubble */
}

static void chameleon(ctx_t *c)
{
    int g = c->g;
    int rx = lerp(P(4.6), P(7.6), g), ry = lerp(P(3), P(4.2), g), leg = lerp(P(2.4), P(4.6), g);
    bool lying = c->pose == PET_POSE_SLEEP;
    if (lying)
        leg = P(0.5);
    int cx = P(13.5), cy = GROUND - leg - ry * 7 / 10 + breath(c) * P(0.5);
    /* Curled tail: a spiral of segments behind. */
    int s = lerp(60, 100, g);
    static const int8_t CURL[][2] = {{0, 0}, {-8, 4}, {-14, 12}, {-14, 22}, {-8, 28}, {-1, 25}, {0, 18}, {-5, 15}};
    int tx = cx - rx * 8 / 10, ty = cy;
    for (int i = 0; i < 7; i++)
    {
        int x0 = tx + CURL[i][0] * s / 100 * 4 / 4, y0 = ty + CURL[i][1] * s / 100;
        int x1 = tx + CURL[i + 1][0] * s / 100, y1 = ty + CURL[i + 1][1] * s / 100;
        seg(c, x0, y0, x1, y1, i < 3 ? P(2) : P(1.5), PET_C_BODY);
    }
    int lx[2] = {cx - rx * 5 / 10, cx + rx * 5 / 10};
    for (int i = 0; i < 2 && !lying; i++)
    {
        seg(c, lx[i] + P(1.5), cy, lx[i] + P(2.5) - leg_swing(c, i + 1), GROUND - P(0.8), P(1.5), PET_C_SHADE);
    }
    ellipse(c, cx, cy, rx, ry, PET_C_BODY, PET_C_SHADE);
    marks(c, (cx - rx) / 4, (cy - ry) / 4, (cx + rx) / 4, (cy + ry) / 4, 5);
    if (g > 60)
        for (int x = (cx - rx * 6 / 10) / 4; x <= (cx + rx * 5 / 10) / 4; x += 2)
            put(c, x, (cy - ry) / 4, PET_C_ACCENT2);
    for (int i = 0; i < 2 && !lying; i++)
        seg(c, lx[i], cy + ry / 3, lx[i] + P(1) + leg_swing(c, i), GROUND - P(0.8), P(1.6), PET_C_BODY);
    int hr = lerp(P(3), P(3.6), g);
    int hx = cx + rx + hr * 3 / 10, hy = cy - ry * 3 / 10 + (c->pose == PET_POSE_EAT ? P(2) : 0);
    /* Casque, head, turret eye; the tongue when eating. */
    tri(c, hx - hr, hy - hr * 2 / 10, hx + hr * 2 / 10, hy - hr * 6 / 10, hx - hr * 9 / 10, hy - hr - lerp(P(1), P(3), g), PET_C_ACCENT2);
    ellipse(c, hx, hy, hr, hr * 85 / 100, PET_C_BODY, PET_C_SHADE);
    if (c->pose == PET_POSE_EAT && (c->frame & 1))
        seg(c, hx + hr, hy + P(1), hx + hr + P(5), hy + P(0.5), P(0.8), PET_C_CHEEK);
    int ex = (hx + hr * 1 / 10) / 4, ey = (hy - hr * 3 / 10) / 4;
    ellipse(c, ex * 4 + 2, ey * 4 + 4, P(1.6), P(1.6), PET_C_BELLY, 0);
    eye(c, ex, ey, false);
}

static void gecko(ctx_t *c)
{
    int g = c->g;
    int rx = lerp(P(4.6), P(7.6), g), ry = lerp(P(2.2), P(3), g);
    bool lying = c->pose == PET_POSE_SLEEP;
    int cx = P(13), cy = GROUND - (lying ? P(0.5) : lerp(P(1.8), P(2.6), g)) - ry + breath(c) * P(0.25);
    int tl = lerp(P(5), P(10), g);
    int curve = (c->pose == PET_POSE_WALK || c->pose == PET_POSE_HAPPY) ? ((c->frame & 2) ? P(1) : -P(1)) : 0;
    int tx = cx - rx * 8 / 10;
    seg(c, tx, cy, tx - tl / 2, cy + P(1) + curve, lerp(P(2.4), P(3), g), PET_C_BODY);
    seg(c, tx - tl / 2, cy + P(1) + curve, tx - tl, cy + P(0.5) - curve, P(1.3), PET_C_BODY);
    int lx[2] = {cx - rx * 5 / 10, cx + rx * 5 / 10};
    for (int i = 0; i < 2 && !lying; i++)
    {
        int x = lx[i] + P(1.2) - leg_swing(c, i + 1);
        seg(c, lx[i] + P(1.2), cy, x + P(1), GROUND - P(0.8), P(1.4), PET_C_SHADE);
    }
    ellipse(c, cx, cy, rx, ry, PET_C_BODY, PET_C_SHADE);
    marks(c, (tx - tl) / 4, (cy - ry) / 4, (cx + rx) / 4, (cy + ry) / 4, 11);
    for (int i = 0; i < 2 && !lying; i++)
    {
        int x = lx[i] + leg_swing(c, i);
        seg(c, lx[i], cy + ry / 3, x + P(1), GROUND - P(0.8), P(1.5), PET_C_BODY);
        seg(c, x, GROUND - P(0.6), x + P(2.2), GROUND - P(0.6), P(1.2), PET_C_ACCENT);
    }
    int hr = lerp(P(3.4), P(3.8), g);
    int hx = cx + rx + hr * 4 / 10, hy = cy - P(0.5) + (c->pose == PET_POSE_EAT ? P(1.5) : 0);
    ellipse(c, hx, hy, hr, hr * 72 / 100, PET_C_BODY, PET_C_SHADE);
    if (c->eyes != 1)
        put(c, (hx + hr * 6 / 10) / 4, (hy + hr * 3 / 10) / 4, PET_C_OUTLINE);   /* smile */
    int ex = (hx + hr * 1 / 10) / 4, ey = (hy - hr * 75 / 100) / 4;
    eye(c, ex, ey, true);
    if (c->l->cheeks && c->eyes != 1)
        put(c, ex + 2, ey + 2, PET_C_CHEEK);
}

/* Contour: empty (or water) pixels touching the animal become outline. */
static void outline(uint8_t *G)
{
    static uint8_t copy[PET_GRID * PET_GRID];
    memcpy(copy, G, sizeof(copy));
    for (int y = 0; y < PET_GRID; y++)
        for (int x = 0; x < PET_GRID; x++)
        {
            uint8_t v = copy[y * PET_GRID + x];
            if (v != PET_C_CLEAR && v != PET_C_GLASS)
                continue;
            static const int8_t D[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (int k = 0; k < 4; k++)
            {
                int nx = x + D[k][0], ny = y + D[k][1];
                if ((unsigned)nx >= PET_GRID || (unsigned)ny >= PET_GRID)
                    continue;
                uint8_t n = copy[ny * PET_GRID + nx];
                if (n != PET_C_CLEAR && n != PET_C_GLASS && n != PET_C_OUTLINE && (v == PET_C_CLEAR || n != PET_C_SHINE))
                {
                    G[y * PET_GRID + x] = PET_C_OUTLINE;
                    break;
                }
            }
        }
}

void pet_draw(const pet_look_t *l, pet_pose_t pose, uint32_t frame, bool blink, uint8_t grid[PET_GRID * PET_GRID])
{
#ifdef WATCH_PET_POKEMON
    /* Petland has Idle frames only; sleep is still, care uses scene effects. */
    if (l->pokemon_dex && pet_pokemon_draw(l->pokemon_dex, l->pokemon_shiny,
                                          pose == PET_POSE_SLEEP ? 0 : frame, grid)) return;
#endif
    memset(grid, PET_C_CLEAR, PET_GRID * PET_GRID);
    ctx_t c = {.l = l, .G = grid, .g = l->growth, .frame = (int)(frame & 0xFFFF), .pose = pose};
    c.eyes = pose == PET_POSE_SLEEP ? 1 : pose == PET_POSE_HAPPY ? 2 : pose == PET_POSE_SAD ? 3 : blink ? 1 : 0;
    if (pose == PET_POSE_HAPPY && (frame & 1))
        c.oy = -2;
    switch (l->species)
    {
    case PET_DOG: dog(&c); break;
    case PET_CAT: cat(&c); break;
    case PET_FISH: fish(&c); break;
    case PET_CHAMELEON: chameleon(&c); break;
    case PET_GECKO: gecko(&c); break;
    case PET_HEN: hen(&c); break;
    case PET_GOAT: goat(&c); break;
    default: bunny(&c); break;
    }
    outline(grid);
}

/* ------------------------------------------------------------ qualities */

static const pet_quality_t QUALITIES[PET_TRAITS][3] = {
    {{"Esploratore", "Annusa ogni angolo e trova sempre qualcosa."},
     {"Attento", "Osserva prima di avvicinarsi."},
     {"Riservato", "Preferisce restare vicino a te."}},
    {{"Instancabile", "Corre e salta tutto il giorno."},
     {"Costante", "Alterna giochi e pisolini."},
     {"Dormiglione", "Un pisolino non si rifiuta mai."}},
    {{"Socievole", "Corre incontro a ogni ospite."},
     {"Gentile", "Saluta con calma chi arriva."},
     {"Timido", "Ci mette un po' a fare amicizia."}},
    {{"Goloso", "Fa festa appena vede la ciotola."},
     {"Misurato", "Mangia il giusto."},
     {"Schizzinoso", "Assaggia e poi ci pensa."}},
    {{"Giocherellone", "Ogni momento è buono per giocare."},
     {"Allegro", "Gioca volentieri se lo inviti."},
     {"Tranquillo", "Ama le giornate calme."}},
};

int pet_qualities(uint64_t seed, const uint8_t values[PET_TRAITS], int level, int days, pet_quality_t out[PET_TRAITS],
                  int order[PET_TRAITS])
{
    for (int i = 0; i < PET_TRAITS; i++)
    {
        int band = values[i] >= 70 ? 0 : values[i] <= 40 ? 2 : 1;
        out[i] = QUALITIES[i][band];
        order[i] = i;
    }
    rng_t r = {seed ^ 0x5045545155414C31ULL};   /* "PETQUAL1" */
    for (int i = PET_TRAITS - 1; i > 0; i--)
    {
        int j = (int)pick(&r, (uint32_t)i + 1);
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    return (level >= 2) + (level >= 4) + (level >= 7) + (level >= 11) + (days >= 3);
}
