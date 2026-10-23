/* Three continuing adventures on the common host. Logical snapshots contain
 * only bounded integers; the large static scenery is a disposable PSRAM cache.
 * Touch movement, knob controls, collisions, upgrades and enemy AI are pure C. */
#include "../minigame_canvas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENEMIES 12
#define SHOTS 14
#define BANDS 6
#define BG_W 256
#define BG_H 120
enum { ARCHER, OCEAN, DEFENCE };
typedef struct { int kind, x, y, vx, vy, hp, age, aux; } entity_t;
typedef struct
{
    uint32_t random;
    int mode, stage, clock, room_clock, bank, total, weapon, armour, skill;
    int x, y, target_x, target_y, hp, air, energy, held, heading, lane;
    int invulnerable, fire, spawn, spawned, collected, intermission, rescue;
    int choice, flash, message, pulse, tower[3], tower_fire[3];
    entity_t enemies[ENEMIES], shots[SHOTS];
} adventure_model_t;
typedef struct
{
    adventure_model_t m;
    uint16_t background[BG_W * BG_H];
    int cached_stage, width, height;
    mg_rect_t previous[BANDS];
    bool ready;
} adventure_t;
_Static_assert(sizeof(adventure_t) <= MG_MAX_STATE_BYTES, "Adventure PSRAM budget");
_Static_assert(sizeof(adventure_model_t) <= MG_CHECKPOINT_BYTES, "Logical checkpoint budget");

static int bound(int x, int a, int b) { return x < a ? a : x > b ? b : x; }
static int triangle(int t) { t = (t % 2000 + 2000) % 2000; return t < 1000 ? t - 500 : 1500 - t; }
static int random_between(adventure_model_t *m, int a, int b) { return a + (int)(mg_random(&m->random) % (unsigned)(b - a + 1)); }
static int max_hp(const adventure_model_t *m) { return 4 + m->armour; }
static int difficulty(const adventure_model_t *m) { return bound(m->stage, 1, 60); }
static bool boss_room(const adventure_model_t *m) { return m->stage % 5 == 0; }
static void add_shot(adventure_model_t *m, int kind, int x, int y, int vx, int vy, int damage)
{
    for (int i = 0; i < SHOTS; i++) if (!m->shots[i].kind)
    {
        m->shots[i] = (entity_t){kind, x, y, vx, vy, damage, 0, 0};
        break;
    }
}
static void aim_shot(adventure_model_t *m, int kind, int x, int y, int tx, int ty, int speed, int damage)
{
    int dx = tx - x, dy = ty - y;
    int scale = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (!scale) scale = 1;
    add_shot(m, kind, x, y, dx * speed / scale, dy * speed / scale, damage);
}
static void spawn_enemy(adventure_model_t *m, int index, int kind)
{
    int d = difficulty(m);
    entity_t *e = &m->enemies[index];
    int hp = 2 + d / 3;
    if (m->mode == ARCHER)
    {
        *e = (entity_t){kind, random_between(m, 550, 880) * 16,
            random_between(m, 180, 820) * 16, 0, 0, kind == 4 ? hp * 5 : hp, 0, random_between(m, 0, 1000)};
    }
    else
    {
        int lane = random_between(m, 0, 2);
        *e = (entity_t){kind, 1080 * 16, (m->mode == DEFENCE ? 220 + lane * 280 : random_between(m, 140, 860)) * 16,
            -(65 + d * 2 + random_between(m, 0, 35)), 0, kind == 4 ? hp * 6 : hp, 0, lane};
    }
}
static void begin_room(adventure_model_t *m)
{
    memset(m->enemies, 0, sizeof(m->enemies));
    memset(m->shots, 0, sizeof(m->shots));
    m->room_clock = m->spawned = m->collected = m->spawn = m->fire = 0;
    m->intermission = m->held = 0;
    m->x = m->target_x = (m->mode == ARCHER ? 180 : 200) * 16;
    m->y = m->target_y = 500 * 16;
    if (m->mode == ARCHER)
    {
        int count = boss_room(m) ? 1 : 3 + difficulty(m) / 12;
        for (int i = 0; i < count; i++) spawn_enemy(m, i, boss_room(m) ? 4 : 1 + i % 3);
    }
}
static void init(void *state, uint32_t seed, int mode)
{
    adventure_t *s = state;
    adventure_model_t *m = &s->m;
    m->random = seed ? seed : 1;
    m->mode = mode;
    m->stage = m->weapon = 1;
    m->bank = 12;
    m->air = m->energy = 100000;
    m->hp = max_hp(m);
    for (int i = 0; i < 3; i++) m->tower[i] = 1;
    begin_room(m);
}
static void archer_start(void *s, uint32_t seed) { init(s, seed, ARCHER); }
static void ocean_start(void *s, uint32_t seed) { init(s, seed, OCEAN); }
static void defence_start(void *s, uint32_t seed) { init(s, seed, DEFENCE); }
static void damage_player(adventure_model_t *m)
{
    if (m->invulnerable || m->rescue || m->intermission) return;
    m->hp--;
    m->invulnerable = 1200;
    m->flash = 700;
    m->message = 1;
    if (m->hp <= 0)
    {
        m->rescue = 1800;
        m->held = 0;
        m->bank -= m->bank / 10;
        m->message = 2;
    }
}
static void clear_room(adventure_model_t *m)
{
    m->intermission = 1;
    m->held = 0;
    m->bank = bound(m->bank + 5 + (boss_room(m) ? 10 : 0), 0, 100000000);
    m->total = bound(m->total + 1, 0, 100000000);
    m->choice = 0;
    m->flash = 1500;
    m->message = 3;
    memset(m->shots, 0, sizeof(m->shots));
}
static void upgrade(adventure_model_t *m, int choice)
{
    int cost = choice == 0 ? 4 + m->weapon * 3 : choice == 1 ? 5 + m->armour * 4 : choice == 2 ? 6 + m->skill * 4 : 0;
    if (m->bank < cost) { m->message = 5; m->flash = 1500; return; }
    m->bank -= cost;
    if (choice == 0) m->weapon = bound(m->weapon + 1, 1, 30);
    else if (choice == 1) m->armour = bound(m->armour + 1, 0, 20);
    else if (choice == 2) m->skill = bound(m->skill + 1, 0, 20);
    m->hp = max_hp(m);
    m->air = m->energy = 100000;
    m->stage = m->stage < 100000000 ? m->stage + 1 : 1;
    begin_room(m);
    m->flash = 1200;
    m->message = 4;
}
static void buy_tower(adventure_model_t *m)
{
    int cost = 3 + m->tower[m->lane] * 3;
    if (m->bank >= cost && m->tower[m->lane] < 20)
    {
        m->bank -= cost;
        m->tower[m->lane]++;
        m->pulse = 500;
    }
    else m->flash = 900, m->message = 5;
}
static void sonar(adventure_model_t *m)
{
    if (m->energy < 30000 || m->pulse > 0) return;
    m->energy -= 30000;
    m->pulse = 700;
    for (int i = 0; i < ENEMIES; i++)
    {
        entity_t *e = &m->enemies[i];
        if (!e->kind || abs(e->x - m->x) > 420 * 16) continue;
        if (e->kind == 1) m->bank++, m->collected++, e->kind = 0;
        else if (e->kind == 2) m->air = bound(m->air + 25000, 0, 100000), e->kind = 0;
        else e->vx = -450, e->hp--;
    }
}
static void input(void *state, const mg_input_t *event)
{
    adventure_model_t *m = &((adventure_t *)state)->m;
    if (m->rescue) return;
    if (m->intermission)
    {
        if (event->kind == MG_TURN) m->choice = bound(m->choice + event->direction, 0, 3);
        else if (event->kind == MG_PRESS || (event->kind == MG_MOVE && m->held))
            m->choice = bound(event->x / 250, 0, 3), m->held = 1;
        else if (event->kind == MG_RELEASE)
        {
            bool buy = m->held && !event->cancelled;
            m->held = 0;
            if (buy) upgrade(m, m->choice);
        }
        else if (event->kind == MG_CLICK) upgrade(m, m->choice);
        return;
    }
    if (event->kind == MG_PRESS || event->kind == MG_MOVE)
    {
        m->held = 1;
        m->target_x = bound(event->x, 100, 900) * 16;
        m->target_y = bound(event->y, 140, 860) * 16;
        m->lane = bound(event->y * 3 / 1000, 0, 2);
    }
    else if (event->kind == MG_RELEASE)
    {
        if (m->held && !event->cancelled)
        {
            if (m->mode == OCEAN) sonar(m);
            else if (m->mode == DEFENCE) buy_tower(m);
        }
        m->held = 0;
    }
    else if (event->kind == MG_TURN)
    {
        if (m->mode == ARCHER)
        {
            static const int dx[8] = {1,1,0,-1,-1,-1,0,1};
            static const int dy[8] = {0,1,1,1,0,-1,-1,-1};
            m->heading = (m->heading + event->direction + 8) % 8;
            m->target_x = bound(m->x / 16 + dx[m->heading] * 250, 100, 900) * 16;
            m->target_y = bound(m->y / 16 + dy[m->heading] * 250, 140, 860) * 16;
            m->held = 1;
        }
        else if (m->mode == OCEAN) m->target_y = bound(m->target_y + event->direction * 1600, 140 * 16, 860 * 16);
        else m->lane = bound(m->lane + event->direction, 0, 2);
    }
    else if (event->kind == MG_CLICK)
    {
        if (m->mode == ARCHER) m->held = !m->held;
        else if (m->mode == OCEAN) sonar(m);
        else buy_tower(m);
    }
}
static void move_toward(int *value, int target, int distance)
{
    if (*value < target) *value = *value + distance > target ? target : *value + distance;
    else if (*value > target) *value = *value - distance < target ? target : *value - distance;
}
static void archer_step(adventure_model_t *m, int ms)
{
    if (m->held)
    {
        move_toward(&m->x, m->target_x, 260 * ms * 16 / 1000);
        move_toward(&m->y, m->target_y, 440 * ms * 16 / 1000);
        if (abs(m->x - m->target_x) < 32 && abs(m->y - m->target_y) < 32) m->held = 0;
    }
    int living = 0, nearest = -1, best = 2147483647;
    for (int i = 0; i < ENEMIES; i++)
    {
        entity_t *e = &m->enemies[i];
        if (!e->kind) continue;
        living++;
        e->age = (e->age + ms) % 60000;
        int dx = (e->x - m->x) / 16, dy = (e->y - m->y) / 16;
        int distance = dx * dx + dy * dy;
        if (distance < best) best = distance, nearest = i;
        if (e->kind == 1)
        {
            move_toward(&e->x, m->x, (35 + difficulty(m)) * ms * 16 / 1000);
            move_toward(&e->y, m->y, (45 + difficulty(m)) * ms * 16 / 1000);
        }
        else
        {
            int period = e->kind == 4 ? 1000 : 1900;
            if ((e->age + e->aux) % period < ms)
            {
                aim_shot(m, 2, e->x, e->y, m->x, m->y, 170 + difficulty(m), 1);
                if (e->kind == 4)
                {
                    aim_shot(m, 2, e->x, e->y, m->x, m->y + 2400, 180, 1);
                    aim_shot(m, 2, e->x, e->y, m->x, m->y - 2400, 180, 1);
                }
            }
            e->y = bound(e->y + triangle(m->clock + e->aux) * ms / 500, 140 * 16, 860 * 16);
        }
        if (distance < 85 * 85) damage_player(m);
    }
    if (!living) { clear_room(m); return; }
    m->fire = bound(m->fire - ms, 0, 1000);
    if (!m->held && m->fire <= 0 && nearest >= 0)
    {
        entity_t *target = &m->enemies[nearest];
        int damage = 1 + m->weapon / 3;
        aim_shot(m, 1, m->x, m->y, target->x, target->y, 650, damage);
        if (m->skill >= 2) aim_shot(m, 1, m->x, m->y, target->x, target->y + 1400, 650, damage);
        if (m->skill >= 5) aim_shot(m, 1, m->x, m->y, target->x, target->y - 1400, 650, damage);
        m->fire = bound(500 - m->weapon * 10, 160, 500);
    }
}
static void ocean_step(adventure_model_t *m, int ms)
{
    move_toward(&m->y, m->target_y, (380 + m->skill * 8) * ms * 16 / 1000);
    m->air -= ms * 100 / (100 + m->weapon * 5);
    if (m->air <= 0) { m->air = 12000; damage_player(m); }
    m->spawn -= ms;
    if (m->spawn <= 0)
    {
        for (int i = 0; i < ENEMIES; i++) if (!m->enemies[i].kind)
        {
            m->spawned = (m->spawned + 1) % 1000000;
            int kind = m->spawned % 4 == 0 ? 2 : m->spawned % 3 == 0 ? 3 : 1;
            if (boss_room(m) && m->spawned % 5 == 0) kind = 4;
            spawn_enemy(m, i, kind);
            break;
        }
        m->spawn = bound(1100 - difficulty(m) * 8, 580, 1100);
    }
    for (int i = 0; i < ENEMIES; i++)
    {
        entity_t *e = &m->enemies[i];
        if (!e->kind) continue;
        e->age = (e->age + ms) % 60000;
        e->x += e->vx * ms * 16 / 1000;
        if (e->kind >= 3) e->y = bound(e->y + triangle(e->age) * ms / 250, 1000, 15000);
        if (abs(e->x - m->x) < 100 * 16 && abs(e->y - m->y) < 160 * 16)
        {
            if (e->kind == 1) m->bank++, m->collected++;
            else if (e->kind == 2) m->air = bound(m->air + 30000, 0, 100000);
            else damage_player(m);
            e->kind = 0;
        }
        if (e->x < -1600 || e->hp <= 0) e->kind = 0;
    }
    if (m->collected >= 6) clear_room(m);
}
static void defence_step(adventure_model_t *m, int ms)
{
    m->y = (220 + m->lane * 280) * 16;
    m->spawn -= ms;
    int needed = 6 + difficulty(m) / 6;
    if (m->spawn <= 0 && m->spawned < needed)
    {
        for (int i = 0; i < ENEMIES; i++) if (!m->enemies[i].kind)
        {
            spawn_enemy(m, i, boss_room(m) && !m->spawned ? 4 : 3);
            m->spawned++;
            break;
        }
        m->spawn = bound(1500 - difficulty(m) * 10, 800, 1500);
    }
    int living = 0;
    for (int i = 0; i < ENEMIES; i++)
    {
        entity_t *e = &m->enemies[i];
        if (!e->kind) continue;
        living++;
        e->x += e->vx * ms * 16 / 1000;
        if (e->x < 160 * 16) { damage_player(m); e->kind = 0; }
    }
    for (int lane = 0; lane < 3; lane++)
    {
        m->tower_fire[lane] = bound(m->tower_fire[lane] - ms, 0, 1000);
        if (m->tower_fire[lane] > 0) continue;
        bool target = false;
        for (int i = 0; i < ENEMIES; i++) if (m->enemies[i].kind && m->enemies[i].aux == lane) target = true;
        if (target)
        {
            add_shot(m, 1, 245 * 16, (220 + lane * 280) * 16, 600, 0, m->tower[lane] + m->weapon / 3);
            m->tower_fire[lane] = bound(900 - m->skill * 15, 350, 900);
        }
    }
    if (m->spawned >= needed && !living) clear_room(m);
}
static void step(void *state, int ms)
{
    adventure_model_t *m = &((adventure_t *)state)->m;
    m->clock = (m->clock + ms) % 60000;
    m->flash = bound(m->flash - ms, 0, 2000);
    m->pulse = bound(m->pulse - ms, 0, 1000);
    m->invulnerable = bound(m->invulnerable - ms, 0, 2000);
    if (m->rescue)
    {
        m->rescue -= ms;
        if (m->rescue <= 0)
        {
            m->rescue = 0;
            m->hp = max_hp(m);
            m->air = m->energy = 100000;
            begin_room(m);
            m->invulnerable = 1600;
        }
        return;
    }
    if (m->intermission)
    {
        return;
    }
    m->room_clock = (m->room_clock + ms) % 60000;
    m->energy = bound(m->energy + ms * (2 + m->skill / 4), 0, 100000);
    if (m->mode == ARCHER) archer_step(m, ms);
    else if (m->mode == OCEAN) ocean_step(m, ms);
    else defence_step(m, ms);
    for (int i = 0; i < SHOTS; i++)
    {
        entity_t *p = &m->shots[i];
        if (!p->kind) continue;
        p->x += p->vx * ms * 16 / 1000;
        p->y += p->vy * ms * 16 / 1000;
        p->age += ms;
        if (p->kind == 1)
        {
            for (int n = 0; n < ENEMIES; n++)
            {
                entity_t *e = &m->enemies[n];
                if (e->kind && abs(p->x - e->x) < 70 * 16 && abs(p->y - e->y) < 120 * 16)
                {
                    e->hp -= p->hp;
                    p->kind = 0;
                    if (e->hp <= 0) e->kind = 0, m->bank += 2, m->collected++;
                    break;
                }
            }
        }
        else if (abs(p->x - m->x) < 60 * 16 && abs(p->y - m->y) < 100 * 16) damage_player(m), p->kind = 0;
        if (p->x < -1000 || p->x > 18000 || p->y < -1000 || p->y > 18000 || p->age > 5000) p->kind = 0;
    }
    m->bank = bound(m->bank, 0, 100000000);
}

static uint32_t tint(uint32_t rgb, int brightness)
{
    return (((rgb >> 16) & 255) * (unsigned)brightness / 255 << 16) |
           (((rgb >> 8) & 255) * (unsigned)brightness / 255 << 8) | (rgb & 255) * (unsigned)brightness / 255;
}
static uint16_t scenery(const adventure_model_t *m, int x, int y)
{
    uint32_t hash = (uint32_t)(x / 4) * 374761393u ^ (uint32_t)(y / 5) * 668265263u ^ (uint32_t)m->stage;
    uint32_t color;
    if (m->mode == ARCHER)
    {
        color = ((x / 80 + y / 130) & 1) ? 0x202B45 : 0x25324E;
        if (x < 65 || x > 935 || y < 90 || y > 910)
            color = (x / 60 + y / 80) & 1 ? 0x647292 : 0x4C5772;
        if (x > 930 && y > 360 && y < 640) color = boss_room(m) ? 0xA74765 : 0x69C9B3;
        if ((hash & 1023) < 8 && x > 75 && x < 920) color = 0x384668;
    }
    else if (m->mode == OCEAN)
    {
        color = (uint32_t)(4 + y / 120) << 16 | (uint32_t)(48 - y / 45) << 8 | (uint32_t)(90 - y / 35);
        if (y < 140 && ((x + m->stage * 17) / 65 & 3) == 0) color = 0x19506A;
        int bottom = 900 + triangle(x * 3 + m->stage * 13) / 8;
        if (y > bottom) color = 0x294C54;
        if ((hash & 255) < 5 && y > 720) color = (hash & 256) ? 0xC56BD4 : 0x5ECCA6;
        if ((hash & 1023) < 4 && y < 850) color = 0x4D94B1;
    }
    else
    {
        color = ((x / 70 + y / 110) & 1) ? 0x183E38 : 0x1C453D;
        for (int lane = 0; lane < 3; lane++) if (abs(y - (220 + lane * 280)) < 75) color = 0x66564E;
        if (x < 105) color = ((x / 35 + y / 80) & 1) ? 0x6C7C90 : 0x4C5C73;
        if ((hash & 511) < 6 && x > 110) color = 0x40845C;
    }
    return mg_color(color);
}
static void bounds_add(mg_rect_t bands[BANDS], const mg_canvas_t *c, int x, int y, int rx, int ry)
{
    for (int i = 0; i < BANDS; i++)
    {
        int top = i * c->height / BANDS, bottom = (i + 1) * c->height / BANDS - 1;
        if (y + ry < top || y - ry > bottom) continue;
        if (x - rx < bands[i].x1) bands[i].x1 = x - rx;
        if (x + rx > bands[i].x2) bands[i].x2 = x + rx;
        bands[i].y1 = top;
        bands[i].y2 = bottom;
    }
}
static void restore(adventure_t *s, mg_canvas_t *c, mg_rect_t r)
{
    if (r.x1 > r.x2) return;
    r.x1 = bound(r.x1, 0, c->width - 1); r.x2 = bound(r.x2, 0, c->width - 1);
    for (int y = r.y1; y <= r.y2; y++)
    {
        uint16_t *row = (uint16_t *)(c->pixels + (size_t)y * c->stride);
        for (int x = r.x1; x <= r.x2; x++) row[x] = s->background[(y / 2) * BG_W + x / 2];
    }
}
static void draw(void *state, mg_canvas_t *c)
{
    adventure_t *s = state;
    adventure_model_t *m = &s->m;
    bool changed = !s->ready || s->cached_stage != m->stage || s->width != c->width || s->height != c->height;
    if (changed)
    {
        for (int y = 0; y < (c->height + 1) / 2; y++)
            for (int x = 0; x < (c->width + 1) / 2; x++)
                s->background[y * BG_W + x] = scenery(m, x * 2000 / c->width, y * 2000 / c->height);
        s->ready = true; s->cached_stage = m->stage; s->width = c->width; s->height = c->height;
        restore(s, c, (mg_rect_t){0, 0, c->width - 1, c->height - 1});
    }
    else for (int i = 0; i < BANDS; i++) restore(s, c, s->previous[i]);
    mg_rect_t bands[BANDS];
    for (int i = 0; i < BANDS; i++) bands[i] = (mg_rect_t){c->width, 0, -1, 0};
    if (m->mode == DEFENCE)
    {
        for (int lane = 0; lane < 3; lane++)
        {
            int x = c->width / 5, y = (220 + lane * 280) * c->height / 1000;
            mg_box_pixels(c, x - 11, y - 11, 23, 23, lane == m->lane ? 0xC7FFCF : 0x718898);
            mg_box_pixels(c, x - 6, y - 7, 17 + bound(m->tower[lane], 1, 10), 14, 0x284D62);
            bounds_add(bands, c, x + 7, y, 28, 14);
        }
    }
    for (int i = 0; i < ENEMIES; i++)
    {
        entity_t *e = &m->enemies[i];
        if (!e->kind) continue;
        int x = e->x * c->width / 16000, y = e->y * c->height / 16000;
        int r = e->kind == 4 ? 16 : 8;
        uint32_t color = e->kind == 4 ? 0xEF6DAF : e->kind == 1 ? 0xFFD975 : e->kind == 2 ? 0x79F4E5 : 0xB796FF;
        if (m->mode == OCEAN && e->kind < 3)
        {
            mg_box_pixels(c, x - 4, y - 7, 9, 15, color);
            mg_box_pixels(c, x - 7, y - 3, 15, 7, tint(color, 180));
        }
        else
        {
            mg_box_pixels(c, x - r, y - r, r * 2, r * 2, tint(color, 110));
            mg_box_pixels(c, x - r + 3, y - r + 2, r * 2 - 6, r * 2 - 5, color);
            mg_box_pixels(c, x - 4, y - 2, 2, 3, 0x142139);
            mg_box_pixels(c, x + 3, y - 2, 2, 3, 0x142139);
            mg_box_pixels(c, x - r, y - r - 5, bound(e->hp, 0, 30) * r / 15 + 1, 2, 0xFFCD8B);
        }
        bounds_add(bands, c, x, y - 2, r + 2, r + 7);
    }
    for (int i = 0; i < SHOTS; i++) if (m->shots[i].kind)
    {
        entity_t *p = &m->shots[i];
        int x = p->x * c->width / 16000, y = p->y * c->height / 16000;
        mg_box_pixels(c, x - 3, y - 2, 7, 5, p->kind == 1 ? 0xACFFCE : 0xFF837C);
        bounds_add(bands, c, x, y, 5, 4);
    }
    int px = m->x * c->width / 16000, py = m->y * c->height / 16000;
    if (!m->invulnerable || (m->clock / 100 & 1)) mg_pet_pixels(c, px, py + c->actor_size / 2);
    bounds_add(bands, c, px, py, c->actor_size / 2 + 4, c->actor_size / 2 + 4);
    if (m->pulse > 0)
    {
        int radius = (700 - m->pulse) * 80 / 700 + 8;
        for (int i = 0; i < 12; i++)
        {
            static const int dx[12] = {100,87,50,0,-50,-87,-100,-87,-50,0,50,87};
            static const int dy[12] = {0,50,87,100,87,50,0,-50,-87,-100,-87,-50};
            int x = px + dx[i] * radius / 100, y = py + dy[i] * radius / 100;
            mg_box_pixels(c, x, y, 3, 3, 0x8AFFEC);
        }
        bounds_add(bands, c, px, py, radius + 5, radius + 5);
    }
    if (m->intermission)
    {
        for (int i = 0; i < 4; i++)
        {
            int x = c->width * (155 + i * 230) / 1000;
            int y = c->height / 2, w = c->width / 6, h = c->height / 2;
            mg_box_pixels(c, x - w / 2 - 2, y - h / 2 - 2, w + 4, h + 4, m->choice == i ? 0xD4FFE5 : 0x65809A);
            mg_box_pixels(c, x - w / 2, y - h / 2, w, h, i == 0 ? 0x694677 : i == 1 ? 0x286A65 : 0x3A5D94);
            mg_box_pixels(c, x - 4, y - 15, 9, 31, 0xECFFF0);
            if (i != 0) mg_box_pixels(c, x - 15, y - 4, 31, 9, 0xECFFF0);
            if (i == 2) mg_box_pixels(c, x - 20, y + 20, 40, 3, 0x77DEFF);
            bounds_add(bands, c, x, y, w / 2 + 4, h / 2 + 4);
        }
    }
    c->full = changed;
    c->dirty_count = 0;
    for (int i = 0; i < BANDS; i++)
    {
        if (!changed)
        {
            mg_rect_t old = s->previous[i];
            int left = old.x1 < bands[i].x1 ? old.x1 : bands[i].x1;
            int right = old.x2 > bands[i].x2 ? old.x2 : bands[i].x2;
            mg_dirty(c, left, i * c->height / BANDS, right, (i + 1) * c->height / BANDS - 1);
        }
        s->previous[i] = bands[i];
    }
}
static void hud(const void *state, char text[MG_HUD_BYTES])
{
    const adventure_model_t *m = &((const adventure_t *)state)->m;
    if (m->intermission)
    {
        snprintf(text, MG_HUD_BYTES, "Oro %d  Costi %d/%d/%d/0", m->bank, 4 + m->weapon * 3, 5 + m->armour * 4, 6 + m->skill * 4);
        return;
    }
    if (m->mode == ARCHER) snprintf(text, MG_HUD_BYTES, "Piano %d  Stanza %d/5  Vita %d  Oro %d", (m->stage - 1) / 5 + 1, (m->stage - 1) % 5 + 1, m->hp, m->bank);
    else if (m->mode == OCEAN) snprintf(text, MG_HUD_BYTES, "Abisso %d  Cristalli %d/6  Aria %d%%  Vita %d", m->stage, m->collected, m->air / 1000, m->hp);
    else snprintf(text, MG_HUD_BYTES, "Ondata %d  Mura %d  Oro %d  Torre %d", m->stage, m->hp, m->bank, m->tower[m->lane]);
}
static const char *feedback(const void *state)
{
    const adventure_model_t *m = &((const adventure_t *)state)->m;
    if (m->rescue) return "Soccorso! Conservi i potenziamenti";
    if (m->intermission) return m->message == 5 && m->flash > 0 ? "Oro insufficiente: riposa o cambia" :
        m->mode == OCEAN ? "Aria / vita / sonar / riposo" : "Arma / vita / abilita / riposo";
    if (m->flash > 0)
    {
        if (m->message == 1) return "Colpito! Cerca una via sicura";
        if (m->message == 4) return "Potenziamento permanente!";
        if (m->message == 5) return "Servono altre monete";
    }
    if (m->mode == ARCHER) return boss_room(m) ? "BOSS! Schiva, fermati e spara" : m->held ? "In movimento: schiva!" : "Fermo: attacco automatico";
    if (m->mode == OCEAN) return m->air < 25000 ? "Aria bassa: cerca le bolle azzurre!" : "Cristalli, bolle d'aria e sonar";
    return boss_room(m) ? "Assedio! Arriva il guardiano" : "Tocca una corsia per potenziarla";
}
static bool result(const void *state, mg_result_t *out) { (void)state; *out = (mg_result_t){0}; return false; }
static bool save(const void *state, void *data, size_t size)
{
    if (size != sizeof(adventure_model_t)) return false;
    memcpy(data, &((const adventure_t *)state)->m, size);
    ((adventure_model_t *)data)->held = 0;
    return true;
}
static bool load(void *state, const void *data, size_t size)
{
    if (size != sizeof(adventure_model_t)) return false;
    const adventure_model_t *m = data;
    adventure_t *s = state;
    if (m->mode != s->m.mode || m->stage < 1 || m->stage > 100000000 || m->bank < 0 || m->bank > 100000000 ||
        m->weapon < 1 || m->weapon > 30 || m->armour < 0 || m->armour > 20 || m->skill < 0 || m->skill > 20 ||
        m->hp < 0 || m->hp > 24 || m->air < 0 || m->air > 100000 || m->energy < 0 || m->energy > 100000 ||
        m->x < 0 || m->x > 16000 || m->y < 0 || m->y > 16000 || m->lane < 0 || m->lane > 2 ||
        m->intermission < 0 || m->intermission > 6000 || m->rescue < 0 || m->rescue > 1800 ||
        m->clock < 0 || m->clock >= 60000 || m->heading < 0 || m->heading > 7 || m->choice < 0 || m->choice > 3) return false;
    if (m->target_x < 0 || m->target_x > 16000 || m->target_y < 0 || m->target_y > 16000 ||
        m->invulnerable < 0 || m->invulnerable > 2000 || m->pulse < 0 || m->pulse > 700 ||
        m->flash < 0 || m->flash > 2000 || m->held < 0 || m->held > 1 ||
        m->spawned < 0 || m->spawned > 1000000 || m->collected < 0 || m->collected > 1000000 ||
        m->room_clock < 0 || m->room_clock >= 60000 || (m->hp == 0 && !m->rescue)) return false;
    for (int i = 0; i < 3; i++) if (m->tower[i] < 1 || m->tower[i] > 20) return false;
    for (int i = 0; i < ENEMIES + SHOTS; i++)
    {
        const entity_t *e = i < ENEMIES ? &m->enemies[i] : &m->shots[i - ENEMIES];
        if (e->kind < 0 || e->kind > 4 || e->x < -100000 || e->x > 100000 || e->y < -100000 || e->y > 100000 ||
            e->vx < -2000 || e->vx > 2000 || e->vy < -2000 || e->vy > 2000 || e->hp < -100 || e->hp > 1000 ||
            e->aux < 0 || e->aux > 2000 || e->age < 0 || e->age > 60000) return false;
    }
    s->m = *m;
    s->m.held = 0;
    s->ready = false;
    return true;
}
#define ADVENTURE(ID, TITLE, INSTRUCTIONS, START, SKY) \
    .id = ID, .title = TITLE, .instructions = INSTRUCTIONS, .sky = SKY, .ground = 0x101F30, \
    .lose_text = "Viaggio sospeso", .win_text = "Nuova scoperta!", .actor_walk = true, \
    .state_size = sizeof(adventure_t), .start = START, .step = step, .input = input, \
    .draw = draw, .hud = hud, .result = result, .feedback = feedback, .continuous = true, \
    .checkpoint_version = 1, .checkpoint_bytes = sizeof(adventure_model_t), .save = save, .load = load
const mg_game_t mg_archer = {ADVENTURE("archer", "Arcipet", "Muoviti; fermati per sparare", archer_start, 0x172137)};
const mg_game_t mg_ocean = {ADVENTURE("ocean", "Abissi luminosi", "Trascina per nuotare; lascia: sonar", ocean_start, 0x06304F)};
const mg_game_t mg_defence = {ADVENTURE("defence", "Bastione dei sogni", "Tocca una corsia: potenzia la torre", defence_start, 0x173B35)};
