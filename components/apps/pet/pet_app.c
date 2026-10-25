/* The pets' little world (docs/ORBI-PETS.md): pixel-art animals that grow,
 * a sky that follows the real weather, guests that arrive and leave through a
 * portal, Orbi visiting while the companion is connected.
 * One lv_timer drives the scene; it does nothing when the page is hidden, the
 * screen is off or a finger is down. Every buffer lives in PSRAM. */
#include "apps_internal.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "hardware/board.h"
#include "link/link_internal.h"
#include "orbi/orbi_view.h"
#include "orbi/pet_sprite.h"
#include "orbi/pet_pokemon.h"
#include "pet.h"
#include "pet_game.h"
/* LVGL 9.6 keeps image-cache invalidation in its private umbrella header. */
#include "lvgl_private.h"
#include "services/ecos.h"
#include "services/weather.h"
#include "ui/ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "pet";

#define TICK_MS     66
#define ACTORS      3
#define FX          12
#define DROPS       14
#define ORBI_FATHER 0x0d0000bb00000245ULL
#define PET_SLOTS_VISIBLE 8

/* ------------------------------------------------------------ rendering */

static EXT_RAM_BSS_ATTR uint8_t s_grid[PET_GRID * PET_GRID];   /* LVGL task only */

typedef struct
{
    lv_draw_buf_t *buf;
    int k;
} canvas_t;

static void canvas_deleted(lv_event_t *e)
{
    canvas_t *c = lv_event_get_user_data(e);
    /* Detach while the descriptor is still alive; cache invalidation may read
     * its header. These manually painted buffers only need an image widget. */
    lv_image_set_src(lv_event_get_target_obj(e), NULL);
    lv_image_cache_drop(c->buf);
    lv_draw_buf_destroy(c->buf);
    free(c);
}

static lv_obj_t *canvas_new(lv_obj_t *parent, int w, int h, int k)
{
    canvas_t *c = calloc(1, sizeof(*c));
    lv_draw_buf_t *buf = c ? lv_draw_buf_create(w * k, h * k, LV_COLOR_FORMAT_ARGB8888, 0) : NULL;
    if (!buf)
    {
        free(c);
        return NULL;
    }
    c->buf = buf;
    c->k = k;
    lv_obj_t *o = lv_image_create(parent);
    lv_image_set_src(o, buf);
    lv_obj_add_event_cb(o, canvas_deleted, LV_EVENT_DELETE, c);
    lv_obj_set_user_data(o, c);
    return o;
}

/* Paints indices through a palette, k x k pixels each, optionally mirrored. */
static void canvas_paint(lv_obj_t *o, const uint8_t *idx, int w, int h, const uint32_t *pal, bool flip)
{
    canvas_t *c = lv_obj_get_user_data(o);
    int k = c->k;
    uint32_t stride = c->buf->header.stride;
    for (int y = 0; y < h; y++)
    {
        uint32_t *row = (uint32_t *)(c->buf->data + (size_t)y * k * stride);
        for (int x = 0; x < w; x++)
        {
            uint32_t v = pal[idx[y * w + (flip ? w - 1 - x : x)]];
            for (int i = 0; i < k; i++)
                row[x * k + i] = v;
        }
        for (int r = 1; r < k; r++)
            memcpy(c->buf->data + ((size_t)y * k + r) * stride, row, (size_t)w * k * 4);
    }
    lv_obj_invalidate(o);
}

static lv_obj_t *sprite_new(lv_obj_t *parent, const pet_look_t *look, int k, pet_pose_t pose, int mood)
{
    lv_obj_t *o = canvas_new(parent, PET_GRID, PET_GRID, k);
    if (!o)
        return NULL;
    uint32_t pal[PET_COLORS];
    pet_palette(look, mood, pal);
    pet_draw(look, pose, 0, false, s_grid);
    canvas_paint(o, s_grid, PET_GRID, PET_GRID, pal, false);
    return o;
}

static pet_look_t look_of(const pet_record_t *p)
{
    uint64_t seed = 0;
    orbi_parse(p->seed, &seed);
    pet_look_t l;
    pet_look(seed, pet_level(p->xp), &l);
    return l;
}

static int mood_of(const pet_record_t *p)
{
    return p->asleep ? 3 : (p->joy < 25 || p->hunger > 80) ? 2 : p->joy > 65 ? 1 : 0;
}

/* 8x8 pixel icons: X colour, o outline, w white. */
typedef enum { ICON_HEART, ICON_FOOD, ICON_BALL, ICON_DROP, ICON_MOON, ICON_MORE, ICON_ZZZ, ICON_STAR, ICONS } icon_t;
static const char *const ICON_ART[ICONS] = {
    ".oo.oo..oXXoXXo.oXwXXXXooXXXXXXo.oXXXXo...oXXo.....oo...........",
    "........ooo.....oXXo.....oXXXo....oXXXo....oXXXo....ooXXo.....oo",
    "..oooo...oXwXXo.oXwXXXXooXXXXXXooXXXXXXooXXXXXXo.oXXXXo...oooo..",
    "...oo......oXo....oXXXo..oXwXXXooXwXXXXooXXXXXXo.oXXXXo...oooo..",
    "..oooo...oXXo...oXXo....oXXo....oXXo....oXXXo..o.oXXXXXo..ooooo.",
    "................................oo.oo.oooXoXoXo.oo.oo.oo........",
    "oooooo..XXXXXo.....oXo.....oXo.....oXo.....oXXXXo..ooooooo......",
    "...oo......oXo...oooXXXoooXXXwXXXo.oXXXXXo..oXXoXXo.oXo..oXo.oo.",
};
static const uint32_t ICON_COLOR[ICONS] = {0xFF5D7A, 0xF2C14E, 0x5AC8FA, 0x6EC8FF, 0xFFE38A, 0xEEF4FB, 0xBFD7FF, 0xFFE070};

static lv_obj_t *icon_new(lv_obj_t *parent, icon_t icon, int k)
{
    lv_obj_t *o = canvas_new(parent, 8, 8, k);
    if (!o)
        return NULL;
    uint8_t idx[64];
    for (int i = 0; i < 64; i++)
    {
        char ch = ICON_ART[icon][i];
        idx[i] = ch == 'X' ? 1 : ch == 'o' ? 2 : ch == 'w' ? 3 : 0;
    }
    uint32_t pal[4] = {0, 0xFF000000u | ICON_COLOR[icon], 0xFF1A1E2Cu, 0xFFFFFFFFu};
    canvas_paint(o, idx, 8, 8, pal, false);
    lv_obj_set_clickable(o, false);
    return o;
}

/* ---------------------------------------------------------------- scene */

enum { A_IDLE, A_WALK, A_ACT, A_ENTER, A_EXIT };

typedef struct
{
    bool used, meeting;
    pet_record_t rec;
    pet_look_t look;
    uint8_t traits[5];
    lv_obj_t *cv;
    uint32_t pal[PET_COLORS];
    int x, target;            /* left edge, pixels x 16 */
    int8_t dir;
    uint8_t state;
    pet_pose_t pose, act_pose;
    int timer, frame, ticks, blink;
    int drawn_pose, drawn_frame, drawn_dir, drawn_blink, drawn_mood;
    int64_t met_at;
} actor_t;

typedef struct
{
    lv_obj_t *o;
    int16_t x, y, vx, vy, life;
} fx_t;

typedef struct
{
    lv_obj_t *screen, *sky, *sun, *ground, *hud_name, *hud_info, *bars[3], *portal, *flash, *menu;
    lv_obj_t *clouds[3], *drops[DROPS];
    lv_timer_t *timer;
    int W, H, k, S, ground_y, margin;
    int weather;              /* 0 clear 1 cloudy 2 rain 3 snow 4 fog 5 storm */
    bool night;
    actor_t actors[ACTORS];
    fx_t fx[FX];
    orbi_view_t *orbi;
    lv_obj_t *orbi_root;
    char selected[17];
    uint32_t ticks;
    int portal_ticks;
} scene_t;

static EXT_RAM_BSS_ATTR scene_t S;

static lv_obj_t *box(lv_obj_t *parent, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, ui_color(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static void fx_spawn(icon_t icon, int x, int y, int vx, int vy, int life)
{
    for (int i = 0; i < FX; i++)
        if (!S.fx[i].o)
        {
            lv_obj_t *o = icon_new(S.screen, icon, S.k > 2 ? 2 : 1);
            if (!o)
                return;
            S.fx[i] = (fx_t){o, (int16_t)x, (int16_t)y, (int16_t)vx, (int16_t)vy, (int16_t)life};
            lv_obj_set_pos(o, x, y);
            return;
        }
}

static void fx_tick(void)
{
    for (int i = 0; i < FX; i++)
    {
        fx_t *f = &S.fx[i];
        if (!f->o)
            continue;
        if (--f->life <= 0)
        {
            lv_obj_delete(f->o);
            f->o = NULL;
            continue;
        }
        f->x += f->vx;
        f->y += f->vy;
        lv_obj_set_pos(f->o, f->x, f->y);
        if (f->life < 6)
            lv_obj_set_style_opa(f->o, (lv_opa_t)(f->life * 40), 0);
    }
}

static int actor_x(const actor_t *a)
{
    return a->x / 16;
}

static void hearts(const actor_t *a, int n)
{
    for (int i = 0; i < n; i++)
        fx_spawn(ICON_HEART, actor_x(a) + S.S / 3 + (int)(esp_random() % (S.S / 3)), S.ground_y - S.S * 9 / 10,
                 (int)(esp_random() % 3) - 1, -2 - (int)(esp_random() % 2), 14 + i * 3);
}

/* ------------------------------------------------------------- weather */

static void sky_paint(void)
{
    weather_t w;
    bool ok = weather_get(&w) && w.valid;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    S.night = ok ? !w.is_day : (tm.tm_hour < 7 || tm.tm_hour >= 20);
    int c = ok ? w.code : 0;
    S.weather = c >= 95 ? 5 : (c >= 71 && c <= 77) || c == 85 || c == 86 ? 3 : (c >= 51 && c <= 67) || (c >= 80 && c <= 82) ? 2
              : c == 45 || c == 48 ? 4 : c >= 2 ? 1 : 0;
    static const uint32_t DAY[6][2] = {{0x4FA8F0, 0xBFE6FF}, {0x7D93AE, 0xC9D5E2}, {0x4E5D70, 0x8C9AAB},
                                       {0x9FB3C8, 0xE6EEF5}, {0xA9B4BE, 0xD8DDE2}, {0x2F3646, 0x5E6676}};
    static const uint32_t NIGHT[6][2] = {{0x0B1330, 0x26365E}, {0x161C28, 0x343C4C}, {0x10151F, 0x2A3140},
                                         {0x1E2838, 0x46546A}, {0x1C2026, 0x3A4048}, {0x0C0F16, 0x262B36}};
    const uint32_t *sky = S.night ? NIGHT[S.weather] : DAY[S.weather];
    lv_obj_set_style_bg_color(S.sky, ui_color(sky[0]), 0);
    lv_obj_set_style_bg_grad_color(S.sky, ui_color(sky[1]), 0);
    bool snow = S.weather == 3;
    uint32_t g0 = snow ? 0xEEF3F8 : 0x6CC453, g1 = snow ? 0xB9C8D8 : 0x3E8E3A;
    if (S.night)
        g0 = (g0 >> 1) & 0x7F7F7F, g1 = (g1 >> 1) & 0x7F7F7F;
    lv_obj_set_style_bg_color(S.ground, ui_color(g0), 0);
    lv_obj_set_style_bg_grad_color(S.ground, ui_color(g1), 0);
    lv_obj_set_style_bg_color(S.sun, ui_color(S.night ? 0xF4F1DE : 0xFFD54A), 0);
    lv_obj_set_hidden(S.sun, S.weather == 2 || S.weather == 5 || S.weather == 4);
    for (int i = 0; i < 3; i++)
    {
        lv_obj_set_hidden(S.clouds[i], S.weather == 0 && i > 0);
        lv_obj_set_style_bg_color(S.clouds[i], ui_color(S.weather >= 2 && S.weather != 3 ? 0x8A94A3 : 0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(S.clouds[i], S.night ? LV_OPA_30 : LV_OPA_80, 0);
    }
    for (int i = 0; i < DROPS; i++)
    {
        lv_obj_set_hidden(S.drops[i], !(S.weather == 2 || S.weather == 3 || S.weather == 5));
        lv_obj_set_size(S.drops[i], snow ? 4 : 2, snow ? 4 : 9);
        lv_obj_set_style_radius(S.drops[i], snow ? 2 : 0, 0);
        lv_obj_set_style_bg_color(S.drops[i], ui_color(snow ? 0xFFFFFF : 0xA8C8F0), 0);
    }
    lv_obj_set_hidden(S.flash, true);
    lv_obj_set_style_bg_opa(S.flash, S.weather == 4 ? LV_OPA_40 : LV_OPA_70, 0);
    if (S.weather == 4)
        lv_obj_set_hidden(S.flash, false);
}

static void weather_tick(void)
{
    for (int i = 0; i < 3; i++)
        if (!lv_obj_is_hidden(S.clouds[i]) && (S.ticks + i * 5) % 6 == 0)
        {
            int x = lv_obj_get_x(S.clouds[i]) + 1;
            lv_obj_set_x(S.clouds[i], x > S.W ? -lv_obj_get_width(S.clouds[i]) : x);
        }
    if (!lv_obj_is_hidden(S.drops[0]))
    {
        int v = S.weather == 3 ? 2 : 9;
        for (int i = 0; i < DROPS; i++)
        {
            int y = lv_obj_get_y(S.drops[i]) + v + (i & 1);
            int x = lv_obj_get_x(S.drops[i]) + (S.weather == 3 ? (int)(esp_random() % 3) - 1 : -1);
            if (y > S.ground_y)
                y = -(int)(esp_random() % (S.H / 3)), x = (int)(esp_random() % S.W);
            lv_obj_set_pos(S.drops[i], x, y);
        }
    }
    if (S.weather == 5)
        lv_obj_set_hidden(S.flash, !(S.ticks % 140 < 2 || S.ticks % 140 == 4));
}

/* --------------------------------------------------------------- actors */

static void command(const char *seed, const char *action, bool quiet)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "action", action);
    cJSON_AddStringToObject(j, "seed", seed);
    const char *error = NULL;
    cJSON *out = pet_command(j, &error);
    if (!out && !quiet && error)
        ui_toast(error);
    cJSON_Delete(j);
    cJSON_Delete(out);
}

static void actor_tapped(lv_event_t *e)
{
    actor_t *a = lv_event_get_user_data(e);
    if (!a->used || a->state == A_EXIT)
        return;
    if (strcmp(S.selected, a->rec.seed))
    {
        pet_select(a->rec.seed);
        snprintf(S.selected, sizeof(S.selected), "%s", a->rec.seed);
        ui_toast("Child selezionato");
        return;
    }
    command(a->rec.seed, "pet", false);
    a->rec.joy = a->rec.joy < 91 ? a->rec.joy + 9 : 100;
    a->state = A_ACT;
    a->act_pose = PET_POSE_HAPPY;
    a->timer = 18;
    hearts(a, 2);
}

static void actor_place(actor_t *a)
{
    lv_obj_set_pos(a->cv, actor_x(a), S.ground_y - 31 * S.k);
}

static actor_t *actor_add(const pet_record_t *p, bool enter)
{
    for (int i = 0; i < ACTORS; i++)
    {
        actor_t *a = &S.actors[i];
        if (a->used)
            continue;
        memset(a, 0, sizeof(*a));
        a->rec = *p;
        a->look = look_of(p);
        pet_traits(p, a->traits);
        a->cv = sprite_new(S.screen, &a->look, S.k, PET_POSE_IDLE, mood_of(p));
        if (!a->cv)
            return NULL;
        lv_obj_set_clickable(a->cv, true);
        lv_obj_add_event_cb(a->cv, actor_tapped, LV_EVENT_SHORT_CLICKED, a);
        a->used = true;
        a->drawn_pose = -1;
        a->dir = 1;
        int span = S.W - 2 * S.margin - S.S;
        a->x = (S.margin + (int)(esp_random() % (span > 1 ? span : 1))) * 16;
        a->state = A_IDLE;
        a->timer = 10 + (int)(esp_random() % 30);
        if (enter)
        {
            a->state = A_ENTER;
            a->x = -S.S * 16;
            a->target = (S.margin + S.S / 2) * 16;
            S.portal_ticks = 50;
            lv_obj_set_hidden(S.portal, false);
            lv_obj_set_pos(S.portal, S.margin / 2 - S.S / 4, S.ground_y - S.S);
        }
        actor_place(a);
        lv_obj_move_foreground(a->cv);
        return a;
    }
    return NULL;
}

static actor_t *other_of(const actor_t *a)
{
    for (int i = 0; i < ACTORS; i++)
        if (S.actors[i].used && &S.actors[i] != a && S.actors[i].state != A_EXIT && !S.actors[i].rec.asleep)
            return &S.actors[i];
    return NULL;
}

static void decide(actor_t *a)
{
    uint32_t r = esp_random() % 100;
    actor_t *o = other_of(a);
    int lo = S.margin * 16, hi = (S.W - S.margin - S.S) * 16;
    a->meeting = false;
    if (S.orbi && r < 45)
        a->target = (S.W / 2 - S.S / 2 + (int)(esp_random() % (S.S / 2 + 1)) - S.S / 4) * 16;
    else if (o && r < (uint32_t)a->traits[2] * 6 / 10)
    {
        int side = actor_x(o) > S.W / 2 ? -1 : 1;
        a->target = o->x + side * S.S * 12;
        a->meeting = true;
    }
    else if (r < 25 + a->traits[0] / 2)
        a->target = lo + (int)(esp_random() % (uint32_t)(hi - lo > 16 ? hi - lo : 16));
    else
    {
        a->timer = 20 + (int)(esp_random() % 50);
        return;
    }
    if (a->target < lo)
        a->target = lo;
    if (a->target > hi)
        a->target = hi;
    a->state = A_WALK;
}

static void actor_tick(actor_t *a)
{
    a->ticks++;
    int step = 1;
    pet_pose_t pose = PET_POSE_IDLE;
    if (a->rec.asleep && a->state != A_EXIT && a->state != A_ENTER)
    {
        pose = PET_POSE_SLEEP;
        if (S.ticks % 40 == 0)
            fx_spawn(ICON_ZZZ, actor_x(a) + S.S * 2 / 3, S.ground_y - S.S * 2 / 3, 1, -1, 18);
    }
    else
        switch (a->state)
        {
        case A_IDLE:
            pose = mood_of(&a->rec) == 2 ? PET_POSE_SAD : PET_POSE_IDLE;
            step = 8;
            if (--a->timer <= 0)
                decide(a);
            break;
        case A_ACT:
            pose = a->act_pose;
            step = 3;
            if (--a->timer <= 0)
                a->state = A_IDLE, a->timer = 20;
            break;
        default:
        {
            pose = PET_POSE_WALK;
            step = 2;
            int speed = S.k * (6 + a->traits[1] / 10);   /* x16 per tick */
            int d = a->target - a->x;
            a->dir = d > 0 ? 1 : d < 0 ? -1 : a->dir;
            if (abs(d) <= speed)
            {
                a->x = a->target;
                if (a->state == A_EXIT)
                {
                    lv_obj_delete(a->cv);
                    a->used = false;
                    lv_obj_set_hidden(S.portal, true);
                    return;
                }
                bool party = a->state == A_ENTER || (S.orbi && !a->meeting);
                a->state = A_ACT;
                a->act_pose = PET_POSE_HAPPY;
                a->timer = 24;
                actor_t *o = other_of(a);
                if (a->meeting && o && abs(o->x - a->x) < S.S * 24)
                {
                    o->state = A_ACT, o->act_pose = PET_POSE_HAPPY, o->timer = 24;
                    o->dir = o->x > a->x ? -1 : 1;
                    a->dir = -o->dir;
                    hearts(a, 1);
                    hearts(o, 1);
                    int64_t now = esp_timer_get_time();
                    if (now - a->met_at > 600LL * 1000000 || !a->met_at)
                    {
                        a->met_at = o->met_at = now;
                        command(a->rec.seed, "meet", true);
                        command(o->rec.seed, "meet", true);
                    }
                }
                else if (party)
                    hearts(a, 2);
                else
                    a->state = A_IDLE, a->timer = 15 + (int)(esp_random() % 40);
            }
            else
                a->x += d > 0 ? speed : -speed;
            actor_place(a);
            break;
        }
        }
    if (a->look.pokemon_dex && step < 4) step = 4;
    if (a->ticks % step == 0 && !(a->look.pokemon_dex && pose == PET_POSE_SLEEP))
        a->frame++;
    if (a->blink > 0)
        a->blink--;
    else if (esp_random() % 60 == 0)
        a->blink = 2;
    int mood = mood_of(&a->rec);
    bool flip = a->dir < 0;
    if (pose != a->drawn_pose || a->frame != a->drawn_frame || flip != a->drawn_dir || (a->blink > 0) != a->drawn_blink ||
        mood != a->drawn_mood)
    {
        if (mood != a->drawn_mood)
            pet_palette(&a->look, mood, a->pal);
        pet_draw(&a->look, pose, (uint32_t)a->frame, a->blink > 0, s_grid);
        canvas_paint(a->cv, s_grid, PET_GRID, PET_GRID, a->pal, flip);
        a->drawn_pose = pose;
        a->drawn_frame = a->frame;
        a->drawn_dir = flip;
        a->drawn_blink = a->blink > 0;
        a->drawn_mood = mood;
    }
}

/* Plays an action on the selected pet: store first, then the animation. */
static actor_t *selected_actor(void)
{
    for (int i = 0; i < ACTORS; i++)
        if (S.actors[i].used && !strcmp(S.actors[i].rec.seed, S.selected))
            return &S.actors[i];
    return NULL;
}

static void hud_update(void)
{
    actor_t *a = selected_actor();
    if (!a)
    {
        lv_label_set_text(S.hud_name, "Il tuo child è in viaggio");
        lv_label_set_text(S.hud_info, "Apri Orbit per ritrovarlo");
        return;
    }
    uint64_t seed;
    orbi_parse(a->rec.seed, &seed);
    orbi_genome_t g;
    orbi_genome(seed, &g);
    pet_look_t updated = look_of(&a->rec);
    if (updated.pokemon_dex != a->look.pokemon_dex || updated.pokemon_shiny != a->look.pokemon_shiny)
        a->drawn_mood = -1;
    a->look = updated;
    lv_label_set_text_fmt(S.hud_name, "%s  LV %d", g.name, a->look.level);
    lv_label_set_text_fmt(S.hud_info, "%s %s - %s", pet_look_name(&a->look), pet_variant_name(&a->look),
                          a->rec.asleep ? "dorme" : pet_stage_name(a->look.stage));
    lv_bar_set_value(S.bars[0], 100 - a->rec.hunger, LV_ANIM_OFF);
    lv_bar_set_value(S.bars[1], a->rec.energy, LV_ANIM_OFF);
    lv_bar_set_value(S.bars[2], a->rec.joy, LV_ANIM_OFF);
}

/* Diff the active pets with the actors: arrivals enter, departures leave. */
static void sync_pets(bool first)
{
    pet_record_t list[PET_SLOTS_VISIBLE];
    int n = pet_list(list, PET_SLOTS_VISIBLE);
    pet_record_t cur;
    if (pet_current(&cur))
        snprintf(S.selected, sizeof(S.selected), "%s", cur.seed);
    for (int i = 0; i < ACTORS; i++)
    {
        actor_t *a = &S.actors[i];
        if (!a->used)
            continue;
        bool found = false;
        for (int j = 0; j < n; j++)
            if (!strcmp(list[j].seed, a->rec.seed))
            {
                found = true;
                bool level_up = pet_level(list[j].xp) > pet_level(a->rec.xp);
                a->rec = list[j];
                if (level_up)
                {
                    a->look = look_of(&a->rec);
                    a->drawn_pose = -1;
                    fx_spawn(ICON_STAR, actor_x(a) + S.S / 2, S.ground_y - S.S, 0, -2, 16);
                }
            }
        if (!found && a->state != A_EXIT)
        {
            a->state = A_EXIT;
            a->target = -S.S * 16;
            S.portal_ticks = 60;
            lv_obj_set_hidden(S.portal, false);
            lv_obj_set_pos(S.portal, S.margin / 2 - S.S / 4, S.ground_y - S.S);
        }
    }
    /* Selected first, then guests, up to three in the scene. */
    for (int pass = 0; pass < 2; pass++)
        for (int j = 0; j < n; j++)
        {
            if ((pass == 0) != !strcmp(list[j].seed, S.selected))
                continue;
            bool shown = false;
            for (int i = 0; i < ACTORS; i++)
                shown |= S.actors[i].used && !strcmp(S.actors[i].rec.seed, list[j].seed);
            if (!shown)
                actor_add(&list[j], pet_take_arrival(list[j].seed) || !first);
        }
    hud_update();
}

static void orbi_sync(void)
{
    bool present = link_orbit_present();
    if (present && !S.orbi)
    {
        orbi_genome_t g;
        orbi_genome(ORBI_FATHER, &g);
        S.orbi = orbi_view_create(S.screen, &g, S.S * 2 / 3);
        if (S.orbi)
        {
            S.orbi_root = orbi_view_root(S.orbi);
            lv_obj_align(S.orbi_root, LV_ALIGN_TOP_MID, 0, S.H * 18 / 100);
            orbi_view_mood(S.orbi, ORBI_LOVE);
            for (int i = 0; i < ACTORS; i++)
                if (S.actors[i].used && S.actors[i].state == A_IDLE)
                    S.actors[i].timer = 1;
        }
    }
    else if (!present && S.orbi)
    {
        lv_obj_delete(S.orbi_root);
        S.orbi = NULL;
        S.orbi_root = NULL;
    }
}

static bool touching(void)
{
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i))
        if (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED)
            return true;
    return false;
}

static void menu_close(lv_event_t *e);
#include "pet_game_ui.inc"

static void tick(lv_timer_t *t)
{
    (void)t;
    if (!S.screen || power_screen() != POWER_SCREEN_ON || app_current() != &pet_app)
    {
        return;
    }
    if (s_game) return;
    if (S.menu || touching())
        return;
    S.ticks++;
    if (S.ticks % 15 == 0)
    {
        sync_pets(false);
        orbi_sync();
    }
    if (S.ticks % 9000 == 0)
    {
        weather_refresh(1800);
        sky_paint();
    }
    weather_tick();
    for (int i = 0; i < ACTORS; i++)
        if (S.actors[i].used)
            actor_tick(&S.actors[i]);
    fx_tick();
    if (S.portal_ticks > 0 && --S.portal_ticks == 0)
        lv_obj_set_hidden(S.portal, true);
    else if (S.portal_ticks > 0)
        lv_obj_set_style_border_opa(S.portal, (lv_opa_t)(120 + (S.ticks % 8) * 15), 0);
}

/* ------------------------------------------------------------- actions */

static void action(lv_event_t *e)
{
    const char *what = lv_event_get_user_data(e);
    actor_t *a = selected_actor();
    if (!a)
        return;
    command(a->rec.seed, what, false);
    pet_current(&a->rec);
    a->state = A_ACT;
    a->timer = 30;
    a->act_pose = PET_POSE_HAPPY;
    int x = actor_x(a) + (a->dir > 0 ? S.S * 3 / 4 : 0), y = S.ground_y - S.S / 2;
    if (!strcmp(what, "feed") || !strcmp(what, "snack"))
    {
        a->act_pose = PET_POSE_EAT;
        fx_spawn(ICON_FOOD, x, y - S.S / 2, 0, 2, 10);
    }
    else if (!strcmp(what, "play"))
        fx_spawn(ICON_BALL, x, y - S.S / 2, a->dir * 3, -3, 16);
    else if (!strcmp(what, "wash"))
        for (int i = 0; i < 3; i++)
            fx_spawn(ICON_DROP, actor_x(a) + i * S.S / 3, S.ground_y - S.S, 0, 1 + i % 2, 14);
    else if (!strcmp(what, "sleep"))
        a->state = A_IDLE;
    else if (!strcmp(what, "train"))
        fx_spawn(ICON_STAR, x, y - S.S / 2, 0, -2, 16);
    hearts(a, 1);
    hud_update();
}

static void menu_close(lv_event_t *e)
{
    (void)e;
    if (s_game == S.menu) s_game = NULL;
    if (S.menu)
        lv_obj_delete(S.menu);
    S.menu = NULL;
}

static void menu_action(lv_event_t *e)
{
    action(e);
    menu_close(e);
}

static void menu_next(lv_event_t *e)
{
    pet_record_t list[PET_SLOTS_VISIBLE];
    int n = pet_list(list, PET_SLOTS_VISIBLE);
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].seed, S.selected))
        {
            pet_select(list[(i + 1) % n].seed);
            break;
        }
    menu_close(e);
    sync_pets(false);
}

static void travel_go(lv_event_t *e)
{
    const char *id = lv_event_get_user_data(e);
    if (pet_travel_send(id, S.selected))
        ui_toast("Preparo il viaggio...");
    else
        ui_toast("Un viaggio e gia in corso");
    menu_close(e);
}

static void travel_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

/* Outing results: the reachable friends and their pets, LVGL locked. */
static void outing_done(const pet_friend_t *friends, int count)
{
    if (!S.screen || S.menu)
        return;
    S.menu = lv_obj_create(S.screen);
    lv_obj_remove_style_all(S.menu);
    lv_obj_set_size(S.menu, S.W, S.H);
    lv_obj_set_style_bg_color(S.menu, ui_color(0x060B12), 0);
    lv_obj_set_style_bg_opa(S.menu, LV_OPA_COVER, 0);
    lv_obj_t *page = ui_page(S.menu, "Uscita");
    for (int i = 0; i < count; i++)
    {
        char *id = malloc(sizeof(friends[i].id));
        if (!id)
            continue;
        memcpy(id, friends[i].id, sizeof(friends[i].id));
        char label[64];
        if (friends[i].pet[0])
            snprintf(label, sizeof(label), "Da %s (%s) su %s", friends[i].pet, pet_species_name(friends[i].species),
                     friends[i].device);
        else
            snprintf(label, sizeof(label), "Su %s", friends[i].device);
        lv_obj_t *b = ui_button(page, label, 0x1F4E5F, travel_go, id);
        lv_obj_add_event_cb(b, travel_free, LV_EVENT_DELETE, id);
    }
    ui_text(page,
            count ? "Scegli chi andare a trovare: il tuo child parte dal portale."
                  : "Nessun amico raggiungibile. Apri la pagina del child sull'altro device (o mettilo in carica) e riprova.",
            true);
    ui_button(page, "Chiudi", 0x24415C, menu_close, NULL);
}

static void outing_start(lv_event_t *e)
{
    menu_close(e);
    ui_toast(pet_outing_search(outing_done) ? "Accendo la radio e cerco amici vicini..." : "Ricerca gia in corso");
}

static void menu_company(lv_event_t *e)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "action", "company");
    cJSON_AddStringToObject(j, "seed", S.selected);
    cJSON_AddBoolToObject(j, "on", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
    const char *error;
    cJSON_Delete(pet_command(j, &error));
    cJSON_Delete(j);
}

static void home_start(lv_event_t *e)
{
    (void)e;
    bool home = pet_at_home(S.selected);
    bool started = !home && pet_travel_home(S.selected);
    menu_close(NULL);
    ui_toast(home ? "Il pet e gia a casa" : started ? "Cerco il dispositivo di origine..."
                                                   : "Un viaggio e gia in corso: riprova dopo");
}

/* Cosmetic appearance is committed by the system worker, never the LVGL task. */
typedef struct { char seed[17]; uint16_t dex; bool shiny, ok; } appearance_job_t;
static bool s_appearance_busy, s_picker_shiny;
static int s_picker_index;
static lv_obj_t *s_picker_image, *s_picker_name;

static void appearance_done(void *arg)
{
    appearance_job_t *job = arg;
    s_appearance_busy = false;
    if (S.screen && app_current() == &pet_app)
    {
        for (int i = 0; i < ACTORS; i++) if (S.actors[i].used)
        {
            S.actors[i].look = look_of(&S.actors[i].rec);
            S.actors[i].drawn_mood = -1;
        }
        menu_close(NULL);
        hud_update();
    }
    ui_toast(job->ok ? "Aspetto salvato" : "Aspetto non salvato: riprova a casa");
    free(job);
}
static void appearance_worker(void *arg)
{
    appearance_job_t *job = arg;
    uint64_t seed;
    pet_record_t pets[PET_SLOTS_VISIBLE];
    int count = pet_list(pets, PET_SLOTS_VISIBLE);
    for (int i = 0; i < count; i++)
        if (!strcmp(pets[i].seed, job->seed) && pet_at_home(job->seed) &&
            !pet_travel_busy() && orbi_parse(job->seed, &seed))
            job->ok = pet_pokemon_set(seed, job->dex, job->shiny);
    /* Retry delivery without losing ownership if the UI queue is momentarily full. */
    while (!ui_async(appearance_done, job)) vTaskDelay(pdMS_TO_TICKS(20));
}
static void appearance_apply(lv_event_t *e)
{
    if (s_appearance_busy) return;
    actor_t *a = selected_actor();
    if (!a || !pet_at_home(a->rec.seed) || pet_travel_busy())
    {
        ui_toast("Scegli l'aspetto quando il tuo child e a casa");
        return;
    }
    appearance_job_t *job = calloc(1, sizeof(*job));
    if (!job) { ui_toast("Memoria insufficiente"); return; }
    snprintf(job->seed, sizeof(job->seed), "%s", a->rec.seed);
    bool original = lv_event_get_user_data(e) != NULL;
    job->dex = original ? 0 : pet_pokemon_dex(s_picker_index);
    job->shiny = !original && s_picker_shiny;
    s_appearance_busy = true;
    if (!sys_post(appearance_worker, job))
    {
        s_appearance_busy = false;
        free(job);
        ui_toast("Sistema occupato: riprova");
    }
}
static void picker_paint(void)
{
    actor_t *a = selected_actor();
    if (!a) return;
    pet_look_t look = a->look;
    look.pokemon_dex = pet_pokemon_dex(s_picker_index);
    look.pokemon_shiny = s_picker_shiny;
    uint32_t palette[PET_COLORS];
    pet_palette(&look, 0, palette);
    pet_draw(&look, PET_POSE_IDLE, 0, false, s_grid);
    canvas_paint(s_picker_image, s_grid, PET_GRID, PET_GRID, palette, false);
    lv_label_set_text_fmt(s_picker_name, "#%03u  %s%s", look.pokemon_dex,
                          pet_look_name(&look), s_picker_shiny ? "  Shiny" : "");
}
static void picker_step(lv_event_t *e)
{
    int direction = (int)(intptr_t)lv_event_get_user_data(e);
    s_picker_index = (s_picker_index + pet_pokemon_count() + direction) % pet_pokemon_count();
    picker_paint();
}
static void picker_shiny(lv_event_t *e)
{
    s_picker_shiny = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    picker_paint();
}
static void appearance_open(lv_event_t *e)
{
    (void)e;
    actor_t *a = selected_actor();
    if (!a || !pet_pokemon_count()) return;
    s_picker_index = 0;
    for (int i = 0; i < pet_pokemon_count(); i++)
        if (pet_pokemon_dex(i) == a->look.pokemon_dex) s_picker_index = i;
    s_picker_shiny = a->look.pokemon_shiny;
    menu_close(NULL);
    S.menu = lv_obj_create(S.screen);
    lv_obj_remove_style_all(S.menu);
    lv_obj_set_size(S.menu, S.W, S.H);
    lv_obj_set_style_bg_color(S.menu, ui_color(0x101B25), 0);
    lv_obj_set_style_bg_opa(S.menu, LV_OPA_COVER, 0);
    lv_obj_t *page = ui_page(S.menu, "Aspetto Pokemon");
    s_picker_image = sprite_new(page, &a->look, 3, PET_POSE_IDLE, 0);
    if (!s_picker_image) { menu_close(NULL); ui_toast("Memoria insufficiente"); return; }
    s_picker_name = lv_label_create(page);
    lv_obj_set_style_text_color(s_picker_name, ui_color(0xFFE6A7), 0);
    lv_obj_set_style_text_font(s_picker_name, UI_FONT_BODY, 0);
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *prev = ui_button(row, "<", 0x24415C, picker_step, (void *)(intptr_t)-1);
    lv_obj_t *next = ui_button(row, ">", 0x24415C, picker_step, (void *)(intptr_t)1);
    lv_obj_set_width(prev, LV_PCT(40));
    lv_obj_set_width(next, LV_PCT(40));
    ui_switch_row(page, LV_SYMBOL_IMAGE, UI_COLOR_BLUE, "Shiny", s_picker_shiny, picker_shiny, NULL);
    ui_button(page, "Usa questo Pokemon", 0x386C52, appearance_apply, NULL);
    ui_button(page, "Aspetto originale", 0x24415C, appearance_apply, (void *)1);
    ui_button(page, "Chiudi", 0x26364D, menu_close, NULL);
    ui_text(page, "Un nuovo aspetto per lo stesso child. Nome, cure e progressi restano tuoi.", true);
    picker_paint();
}

static void menu_open(lv_event_t *e)
{
    (void)e;
    actor_t *a = selected_actor();
    if (S.menu || !a)
        return;
    S.menu = lv_obj_create(S.screen);
    lv_obj_remove_style_all(S.menu);
    lv_obj_set_size(S.menu, S.W, S.H);
    lv_obj_set_style_bg_color(S.menu, ui_color(0x060B12), 0);
    lv_obj_set_style_bg_opa(S.menu, LV_OPA_COVER, 0);
    lv_obj_t *page = ui_page(S.menu, "Il tuo child");
    ui_button(page, "Chiudi", 0x24415C, menu_close, NULL);
    if (pet_pokemon_count() && pet_at_home(S.selected))
        ui_button(page, "Aspetto Pokemon", 0x386C52, appearance_open, NULL);
    ui_button(page, "Minigiochi", 0x1F4E5F, game_menu, NULL);
    if (!pet_at_home(S.selected))
        ui_button(page, "Torna a casa", 0x245544, home_start, NULL);
    ui_button(page, "Snack", 0x245544, menu_action, "snack");
    ui_button(page, "Allenamento", 0x493663, menu_action, "train");

    /* Personality: revealed qualities, the others still to discover. */
    uint64_t seed;
    orbi_parse(a->rec.seed, &seed);
    int days = a->rec.born > 0 ? (int)((time(NULL) - a->rec.born) / 86400) : 0;
    pet_quality_t q[PET_TRAITS];
    int order[PET_TRAITS];
    int shown = pet_qualities(seed, a->traits, a->look.level, days < 0 ? 0 : days, q, order);
    char text[420];
    int len = snprintf(text, sizeof(text), "%s %s%s%s - %s, nato su %s.\n\nPERSONALITA'\n", pet_look_name(&a->look),
                       pet_variant_name(&a->look), a->look.mark && !a->look.pokemon_dex ? " " : "",
                       a->look.pokemon_dex ? "" : pet_mark_name(a->look.mark),
                       pet_stage_name(a->look.stage), a->rec.origin);
    for (int i = 0; i < PET_TRAITS && len < (int)sizeof(text) - 80; i++)
        len += i < shown ? snprintf(text + len, sizeof(text) - len, "%s: %s\n", q[order[i]].name, q[order[i]].about)
                         : snprintf(text + len, sizeof(text) - len, "??? - passate altro tempo insieme\n");
    ui_text(page, text, false);

    /* Discovered breeds. */
    uint16_t keys[64];
    int n = pet_dex(keys, 64), total = 0;
    for (int s = 0; s < PET_SPECIES; s++)
        total += pet_variant_count(s);
    char title[64];
    snprintf(title, sizeof(title), "RAZZE SCOPERTE: %d", n);
    ui_text(page, title, false);
    lv_obj_t *grid = lv_obj_create(page);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(grid, 6, 0);
    for (int i = 0; i < n; i++)
    {
        pet_look_t l = {.species = (uint8_t)(keys[i] >> 8), .variant = (uint8_t)((keys[i] >> 4) & 15),
                        .mark = (uint8_t)(keys[i] & 15), .growth = 255, .level = 15, .stage = 2};
        sprite_new(grid, &l, 2, PET_POSE_IDLE, 0);
    }
    (void)total;
    ui_text(page, "Ogni child che nasce o arriva su questo device aggiunge la sua razza.", true);
    ui_button(page, "Altro child sul device", 0x26364D, menu_next, NULL);

    ui_button(page, "Uscita: cerca amici vicini", 0x1F4E5F, outing_start, NULL);
    ui_text(page, "Accende la radio e cerca i device abbinati in Ecosistema con la pagina del child aperta (o in carica).",
            true);
    ui_switch_row(page, LV_SYMBOL_EYE_OPEN, UI_COLOR_BLUE, "Piccola compagnia", pet_company(), menu_company, NULL);
    ui_text(page, "Viaggi: Orbit > Ecosistema > Children. Identita e cure viaggiano con lui. "
                  "L'aspetto Pokemon e una scelta locale di questo device.", true);
}

static lv_obj_t *round_button(lv_obj_t *parent, icon_t icon, lv_event_cb_t cb, void *data, int size)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, size, size + 18);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, ui_color(0x111A26), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_border_color(b, ui_color(0x34C6F4), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_bg_color(b, ui_color(0x1F2C3D), LV_STATE_PRESSED);
    lv_obj_t *i = icon_new(b, icon, size >= 52 ? 3 : 2);
    if (i)
        lv_obj_align(i, LV_ALIGN_TOP_MID, 0, 9);
    static const char *const LABELS[ICONS] = {"Cura", "Cibo", "Gioco", "Bagno", "Sonno", "Scheda", "", ""};
    lv_obj_t *label = lv_label_create(b);
    lv_label_set_text(label, LABELS[icon]);
    lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(label, ui_color(0xEEF4FB), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    lv_port_set_focusable(b);
    return b;
}

static lv_obj_t *hud_label(int y, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(S.screen);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_color(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, S.W * 7 / 10);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    if (!pet_init())
        ui_toast("Archivio child non disponibile: riprova");
    memset(&S, 0, sizeof(S));
    const board_info_t *b = board_info();
    S.screen = screen;
    s_game = NULL;
    S.W = b->width;
    S.H = b->height;
    S.k = S.W >= 400 ? 5 : S.W >= 300 ? 4 : 2;
    S.S = PET_GRID * S.k;
    S.ground_y = S.H * 68 / 100;
    S.margin = b->round ? S.W / 9 : 8;
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, ui_color(0x060B12), 0);

    S.sky = box(screen, 0x4FA8F0, LV_OPA_COVER);
    lv_obj_set_size(S.sky, S.W, S.ground_y);
    lv_obj_set_style_bg_grad_dir(S.sky, LV_GRAD_DIR_VER, 0);
    S.sun = box(screen, 0xFFD54A, LV_OPA_COVER);
    lv_obj_set_size(S.sun, S.W / 9, S.W / 9);
    lv_obj_set_style_radius(S.sun, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(S.sun, S.W * 66 / 100, S.H * 27 / 100);
    for (int i = 0; i < 3; i++)
    {
        S.clouds[i] = box(screen, 0xFFFFFF, LV_OPA_80);
        lv_obj_set_size(S.clouds[i], S.W / 5 + i * S.W / 20, S.W / 18);
        lv_obj_set_style_radius(S.clouds[i], S.W / 36, 0);
        lv_obj_set_pos(S.clouds[i], i * S.W / 3, S.H * (30 + i * 7) / 100);
    }
    S.flash = box(screen, 0xFFFFFF, LV_OPA_70);
    lv_obj_set_size(S.flash, S.W, S.ground_y);
    for (int i = 0; i < DROPS; i++)
    {
        S.drops[i] = box(screen, 0xA8C8F0, LV_OPA_80);
        lv_obj_set_pos(S.drops[i], (int)(esp_random() % S.W), (int)(esp_random() % S.ground_y));
    }
    S.ground = box(screen, 0x6CC453, LV_OPA_COVER);
    lv_obj_set_size(S.ground, S.W, S.H - S.ground_y);
    lv_obj_set_pos(S.ground, 0, S.ground_y);
    lv_obj_set_style_bg_grad_dir(S.ground, LV_GRAD_DIR_VER, 0);
    for (int i = 0; i < 9; i++)
    {
        lv_obj_t *tuft = box(S.ground, 0x2E6E2C, LV_OPA_50);
        lv_obj_set_size(tuft, S.k * 2, S.k * 2);
        lv_obj_set_pos(tuft, (int)((uint32_t)(i * 7919 + 13) % S.W), S.k * (2 + (i * 5) % 9));
    }
    S.portal = box(screen, 0x34C6F4, LV_OPA_TRANSP);
    lv_obj_set_size(S.portal, S.S / 2, S.S);
    lv_obj_set_style_radius(S.portal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(S.portal, S.k * 2, 0);
    lv_obj_set_style_border_color(S.portal, ui_color(0x7EE8FF), 0);
    lv_obj_set_hidden(S.portal, true);

    lv_obj_t *panel = box(screen, 0x101B25, LV_OPA_70);
    lv_obj_set_size(panel, S.W * 76 / 100, 106);
    lv_obj_set_style_radius(panel, 24, 0);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, (b->round ? S.H / 12 : 10) - 8);
    S.hud_name = hud_label(b->round ? S.H / 12 : 10, ui_font_title, 0xFFFFFF);
    S.hud_info = hud_label((b->round ? S.H / 12 : 10) + 28, UI_FONT_SMALL, 0xEEF4FB);
    static const uint32_t BAR[3] = {0xF2C14E, 0x5AC8FA, 0xFF5D7A};
    int bw = S.W * (b->round ? 16 : 22) / 100;
    for (int i = 0; i < 3; i++)
    {
        S.bars[i] = lv_bar_create(screen);
        lv_obj_set_size(S.bars[i], bw, 6);
        lv_bar_set_range(S.bars[i], 0, 100);
        lv_obj_set_style_bg_color(S.bars[i], ui_color(0x111A26), 0);
        lv_obj_set_style_bg_opa(S.bars[i], LV_OPA_70, 0);
        lv_obj_set_style_bg_color(S.bars[i], ui_color(BAR[i]), LV_PART_INDICATOR);
        lv_obj_align(S.bars[i], LV_ALIGN_TOP_MID, (i - 1) * (bw + 8), (b->round ? S.H / 12 : 10) + 80);
        static const char *const NAMES[3] = {"Sazieta", "Energia", "Felicita"};
        lv_obj_t *caption = lv_label_create(screen);
        lv_label_set_text(caption, NAMES[i]);
        lv_obj_set_style_text_color(caption, ui_color(BAR[i]), 0);
        lv_obj_set_style_text_font(caption, UI_FONT_CAPTION, 0);
        lv_obj_align(caption, LV_ALIGN_TOP_MID, (i - 1) * (bw + 8), (b->round ? S.H / 12 : 10) + 59);
    }

    /* Actions on the ground. */
    int size = S.W >= 400 ? 54 : S.W >= 300 ? 48 : 40;
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, S.W, S.H - S.ground_y);
    lv_obj_set_pos(row, 0, S.ground_y);
    lv_obj_t *buttons[5] = {
        round_button(row, ICON_FOOD, action, "feed", size),
        round_button(row, ICON_BALL, action, "play", size),
        round_button(row, ICON_DROP, action, "wash", size),
        round_button(row, ICON_MOON, action, "sleep", size),
        round_button(row, ICON_MORE, menu_open, NULL, size)
    };
    for (int i = 0; i < 5; i++)
    {
        /* Lift the outer controls into the circle; labels stay visible too. */
        int y = b->round ? (i == 0 || i == 4 ? 2 : S.H * (i == 2 ? 7 : 5) / 100) : 12;
        lv_obj_set_pos(buttons[i], S.W / 2 + (i - 2) * S.W * 15 / 100 - size / 2, y);
    }

    sky_paint();
    weather_refresh(1800);
    sync_pets(true);
    orbi_sync();
    ecos_family_hold(true);   /* reachable for journeys while the page is open */
    pet_travel_resume();
    S.timer = lv_timer_create(tick, TICK_MS, NULL);
    ESP_LOGI(TAG, "scene ready in %d ms, k=%d", (int)((esp_timer_get_time() - t0) / 1000), S.k);
}

static void destroy(void)
{
    s_game = NULL;
    if (S.timer)
        lv_timer_delete(S.timer);
    S.timer = NULL;
    pet_outing_cancel();
    ecos_family_hold(false);
    S.screen = NULL;   /* objects go with the screen */
    S.menu = NULL;
    S.orbi = NULL;
    for (int i = 0; i < FX; i++)
        S.fx[i].o = NULL;
}

const app_t pet_app = {.id = "orbi",
                       .name = "Orbi / Children",
                       .icon = LV_SYMBOL_IMAGE,
                       .color = 0x34C6F4,
                       .flags = APP_FLAG_HIDDEN,
                       .create = create,
                       .destroy = destroy,
                       .back = game_back,
                       .button = game_button,
                       .about = "Il child nato su questo device. Coccole, cibo, gioco, bagnetto e viaggi con Orbit."};

/* ----------------------------------------------------- menu and corner */

static void open(lv_event_t *e)
{
    (void)e;
    app_open("orbi", NULL);
}

void pet_cta(lv_obj_t *parent)
{
    pet_record_t p;
    lv_obj_t *card = lv_obj_create(parent);
    ui_make_card(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, 94);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, true);
    lv_port_set_focusable(card);
    lv_obj_add_event_cb(card, open, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_style_text_font(title, UI_FONT_BODY, 0);
    lv_obj_set_width(title, LV_PCT(65));
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(title, LV_ALIGN_RIGHT_MID, 0, 0);
    if (pet_current(&p))
    {
        uint64_t seed;
        orbi_parse(p.seed, &seed);
        orbi_genome_t g;
        orbi_genome(seed, &g);
        pet_look_t l = look_of(&p);
        lv_label_set_text_fmt(title, "%s - %s\nApri il tuo child  >", g.name, pet_look_name(&l));
        lv_obj_t *s = sprite_new(card, &l, 2, PET_POSE_IDLE, mood_of(&p));
        if (s)
        {
            lv_obj_set_clickable(s, false);
            lv_obj_align(s, LV_ALIGN_LEFT_MID, -6, 0);
        }
    }
    else
        lv_label_set_text(title, "Children\nApri  >");
}

void pet_thumbnail(lv_obj_t *parent, int width)
{
    pet_record_t p;
    if (!pet_current(&p))
        return;
    lv_obj_clean(parent);
    pet_look_t l = look_of(&p);
    int k = width / PET_GRID;
    lv_obj_t *s = sprite_new(parent, &l, k < 1 ? 1 : k, PET_POSE_IDLE, mood_of(&p));
    if (s)
    {
        lv_obj_set_clickable(s, false);
        lv_obj_center(s);
    }
}

static lv_obj_t *s_ambient;
static char s_ambient_seed[17];
static int s_ambient_frame;
static pet_look_t s_ambient_look;

static void ambient(lv_timer_t *timer)
{
    (void)timer;
    pet_record_t p;
    const app_t *app = app_current();
    bool show = pet_company() && power_screen() == POWER_SCREEN_ON &&
                (!app || (app != &pet_app && app != &ring_app && app != &call_app)) && !link_scene_busy() &&
                pet_current(&p);
    if (!show)
    {
        if (s_ambient)
            lv_obj_set_hidden(s_ambient, true);
        return;
    }
    /* Decorative animation never competes with a gesture or page transition. */
    if (lv_anim_count_running() != 0) return;
    for (lv_indev_t *input = lv_indev_get_next(NULL); input != NULL; input = lv_indev_get_next(input))
        if (lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED || lv_indev_get_scroll_obj(input) != NULL) return;
    int k = board_info()->width >= 400 ? 2 : 1;
    if (!s_ambient || strcmp(s_ambient_seed, p.seed))
    {
        if (s_ambient)
            lv_obj_delete(s_ambient);
        s_ambient_look = look_of(&p);
        /* Decorative only: cannot intercept a swipe or a button underneath. */
        s_ambient = canvas_new(lv_layer_top(), PET_GRID, PET_GRID, k);
        snprintf(s_ambient_seed, sizeof(s_ambient_seed), "%s", s_ambient ? p.seed : "");
        if (!s_ambient)
            return;
        lv_obj_set_clickable(s_ambient, false);
        int inset = board_info()->round ? board_info()->width / 7 : 4;
        lv_obj_align(s_ambient, LV_ALIGN_BOTTOM_RIGHT, -inset, -inset);
    }
    lv_obj_set_hidden(s_ambient, false);
    s_ambient_look = look_of(&p);
    uint32_t pal[PET_COLORS];
    pet_palette(&s_ambient_look, mood_of(&p), pal);
    pet_draw(&s_ambient_look, p.asleep ? PET_POSE_SLEEP : PET_POSE_IDLE, (uint32_t)(s_ambient_frame++), false, s_grid);
    canvas_paint(s_ambient, s_grid, PET_GRID, PET_GRID, pal, true);
}

void pet_companion_start(void)
{
    pet_pokemon_init();
    pet_travel_init();
    lv_timer_create(ambient, 1500, NULL);
}
