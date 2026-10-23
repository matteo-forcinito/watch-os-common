/* Quantum Leap: a pet astronaut, orbital aiming, slingshot gravity and a
 * corrective impulse. Rules are integer-only. Static space is cached at
 * half resolution in session PSRAM; each frame restores only moving bounds.
 * The common host supplies controls, timing, actor, result and lifetime. */
#include "../minigame_canvas.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define Q_GOAL 8
#define Q_ORBIT 90
#define Q_CAPTURE 88
#define Q_BACKGROUND_W 256
#define Q_BACKGROUND_H 120
#define Q_PARTICLES 6
#define Q_TRAIL 5

static const int16_t s_sine[64] = {
    0,100,200,297,392,483,569,650,724,792,851,903,946,980,1004,1019,
    1024,1019,1004,980,946,903,851,792,724,650,569,483,392,297,200,100,
    0,-100,-200,-297,-392,-483,-569,-650,-724,-792,-851,-903,-946,-980,-1004,-1019,
    -1024,-1019,-1004,-980,-946,-903,-851,-792,-724,-650,-569,-483,-392,-297,-200,-100};
static const uint32_t s_planet_colors[8] = {
    0x49D8FF,0xB78AFF,0xFFB95A,0x72F4BE,0xFF7DC9,0x75A4FF,0xFFE990,0xB9FFED};

typedef struct { int x, y, vx, vy, age; } q_particle_t;
typedef struct
{
    uint32_t random, seed;
    int elapsed, height, phase, x, y, vx, vy, charge, energy;
    int current_y, target_y, hole_y, flight, jumps, perfect, lives, generation;
    int flash, message, trail_at, cooldown;
    bool orbiting, held, done;
    int trail_x[Q_TRAIL], trail_y[Q_TRAIL];
    q_particle_t particles[Q_PARTICLES];
} quantum_model_t;
typedef struct
{
    quantum_model_t model;
    uint16_t background[Q_BACKGROUND_W * Q_BACKGROUND_H];
    int cached_generation, cached_width, cached_height;
    bool ready, previous_valid;
    mg_rect_t previous;
} quantum_state_t;
_Static_assert(sizeof(quantum_state_t) <= MG_MAX_STATE_BYTES, "Quantum state must fit the shared PSRAM budget");

static int sine(int phase)
{
    unsigned p = (unsigned)phase & 1023;
    int index = (int)(p >> 4), fraction = (int)(p & 15);
    return (s_sine[index] * (16 - fraction) + s_sine[(index + 1) & 63] * fraction) / 16;
}
static int cosine(int phase) { return sine(phase + 256); }
static int clamp(int v, int low, int high) { return v < low ? low : v > high ? high : v; }
static void orbit_position(quantum_model_t *m)
{
    m->x = (250 + cosine(m->phase) * Q_ORBIT / 1024) * 16;
    m->y = (m->current_y + sine(m->phase) * Q_ORBIT / 1024) * 16;
}
static void reset_trail(quantum_model_t *m)
{
    for (int i = 0; i < Q_TRAIL; i++) m->trail_x[i] = m->x, m->trail_y[i] = m->y;
}
static void init_model(quantum_model_t *m, uint32_t seed, int height)
{
    memset(m, 0, sizeof(*m));
    m->seed = m->random = seed ? seed : 1;
    m->height = height;
    m->current_y = m->target_y = height / 2;
    m->hole_y = height / 2;
    m->phase = 768;
    m->energy = 100;
    m->lives = 3;
    m->orbiting = true;
    orbit_position(m);
    reset_trail(m);
}
static void start(void *state, uint32_t seed)
{
    init_model(&((quantum_state_t *)state)->model, seed, 450);
}
static void burst(quantum_model_t *m, bool good)
{
    for (int i = 0; i < Q_PARTICLES; i++)
    {
        int phase = i * 1024 / Q_PARTICLES;
        m->particles[i] = (q_particle_t){m->x, m->y, cosine(phase) * 130 / 1024,
                                       sine(phase) * 130 / 1024, good ? 450 : 300};
    }
}
static void launch(quantum_model_t *m)
{
    if (!m->orbiting || m->done) return;
    int force = 390 + m->charge * 2;
    m->vx = (-sine(m->phase) * force + cosine(m->phase) * m->charge / 2) / 1024;
    m->vy = (cosine(m->phase) * force + sine(m->phase) * m->charge / 2) / 1024;
    m->orbiting = m->held = false;
    m->flight = 0;
    m->message = 0;
    burst(m, true);
}
static void impulse(quantum_model_t *m)
{
    if (m->orbiting || m->energy < 40 || m->cooldown > 0) return;
    int dx = 700 - m->x / 16, dy = m->target_y - m->y / 16;
    int scale = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (!scale) return;
    m->vx += dx * 110 / scale;
    m->vy += dy * 110 / scale;
    m->energy -= 40;
    m->cooldown = 350;
    m->message = 3;
    m->flash = 450;
    burst(m, true);
}
static void input(void *state, const mg_input_t *event)
{
    quantum_model_t *m = &((quantum_state_t *)state)->model;
    if (m->done) return;
    if (event->kind == MG_PRESS)
    {
        m->held = true;
        if (m->orbiting && m->charge < 10) m->charge = 10;
        else if (!m->orbiting) impulse(m);
    }
    else if (event->kind == MG_RELEASE)
    {
        bool charged = m->held && m->charge > 0 && !event->cancelled;
        m->held = false;
        if (event->cancelled) m->charge = 0;
        if (charged && m->orbiting) launch(m);
    }
    else if (event->kind == MG_TURN)
    {
        if (m->orbiting)
        {
            m->phase = (m->phase + event->direction * 24) & 1023;
            orbit_position(m);
        }
        else m->vy = clamp(m->vy + event->direction * 55, -700, 700);
    }
    else if (event->kind == MG_CLICK)
    {
        if (m->orbiting) m->charge = 60, launch(m);
        else impulse(m);
    }
}
static void gravity(const quantum_model_t *m, int x, int y, int *ax, int *ay)
{
    int dx = 700 - x, dy = m->target_y - y;
    int distance = dx * dx + dy * dy + 20000;
    *ax = dx * 90000 / distance;
    *ay = dy * 90000 / distance;
    if (m->jumps >= 3)
    {
        dx = 490 - x;
        dy = m->hole_y - y;
        distance = dx * dx + dy * dy + 9000;
        *ax += dx * 110000 / distance;
        *ay += dy * 110000 / distance;
    }
    *ax = clamp(*ax, -900, 900);
    *ay = clamp(*ay, -900, 900);
}
static void fail(quantum_model_t *m)
{
    m->lives--;
    m->orbiting = true;
    m->held = false;
    m->charge = 0;
    m->phase = 768;
    m->flight = 0;
    m->energy = clamp(m->energy + 30, 0, 100);
    m->message = 2;
    m->flash = 1000;
    orbit_position(m);
    reset_trail(m);
    burst(m, false);
    if (!m->lives) m->done = true;
}
static void capture(quantum_model_t *m)
{
    int dx = m->x / 16 - 700, dy = m->y / 16 - m->target_y;
    int best = -2147483647, phase = 768;
    for (int i = 0; i < 64; i++)
    {
        int dot = dx * cosine(i * 16) + dy * sine(i * 16);
        if (dot > best) best = dot, phase = i * 16;
    }
    if (m->flight < 1800 && m->charge >= 40) m->perfect++;
    m->jumps++;
    m->generation++;
    m->current_y = m->target_y;
    m->target_y = 110 + (int)(mg_random(&m->random) % (unsigned)(m->height - 219));
    m->hole_y = clamp((m->current_y + m->target_y) / 2 + (m->current_y < m->target_y ? -115 : 115), 65, m->height - 65);
    m->phase = phase;
    m->orbiting = true;
    m->held = false;
    m->charge = 0;
    m->energy = clamp(m->energy + 35, 0, 100);
    m->message = 1;
    m->flash = 900;
    orbit_position(m);
    reset_trail(m);
    burst(m, true);
    if (m->jumps >= Q_GOAL) m->done = true;
}
static void model_step(quantum_model_t *m, int ms)
{
    if (m->done) return;
    m->elapsed += ms;
    if (m->flash > 0) m->flash -= ms;
    if (m->cooldown > 0) m->cooldown -= ms;
    for (int i = 0; i < Q_PARTICLES; i++)
        if (m->particles[i].age > 0)
        {
            m->particles[i].age -= ms;
            m->particles[i].x += m->particles[i].vx * ms * 16 / 1000;
            m->particles[i].y += m->particles[i].vy * ms * 16 / 1000;
        }
    if (m->orbiting)
    {
        m->phase = (m->phase + (m->held ? 1 : 6) * ms / 16) & 1023;
        if (m->held) m->charge = clamp(m->charge + ms / 8, 0, 100);
        orbit_position(m);
    }
    else
    {
        m->flight += ms;
        int x = m->x / 16, y = m->y / 16, ax, ay;
        gravity(m, x, y, &ax, &ay);
        m->vx = clamp(m->vx + ax * ms / 1000, -800, 800);
        m->vy = clamp(m->vy + ay * ms / 1000, -800, 800);
        m->x += m->vx * ms * 16 / 1000;
        m->y += m->vy * ms * 16 / 1000;
        x = m->x / 16;
        y = m->y / 16;
        int dx = x - 700, dy = y - m->target_y;
        if (m->flight > 120 && dx * dx + dy * dy <= Q_CAPTURE * Q_CAPTURE) capture(m);
        else
        {
            dx = x - 490;
            dy = y - m->hole_y;
            if (x < -160 || x > 1200 || y < -180 || y > m->height + 180 || m->flight > 4000 ||
                (m->jumps >= 3 && dx * dx + dy * dy < 28 * 28)) fail(m);
        }
    }
    if (m->elapsed - m->trail_at >= 60)
    {
        m->trail_at = m->elapsed;
        for (int i = Q_TRAIL - 1; i > 0; i--) m->trail_x[i] = m->trail_x[i - 1], m->trail_y[i] = m->trail_y[i - 1];
        m->trail_x[0] = m->x;
        m->trail_y[0] = m->y;
    }
    if (m->elapsed >= MG_MAX_DURATION_MS) m->done = true;
}
static void step(void *state, int ms) { model_step(&((quantum_state_t *)state)->model, ms); }
static uint32_t shade(uint32_t rgb, int amount)
{
    return (((rgb >> 16) & 255) * (unsigned)amount / 255 << 16) |
           (((rgb >> 8) & 255) * (unsigned)amount / 255 << 8) | ((rgb & 255) * (unsigned)amount / 255);
}
static uint16_t space_pixel(const quantum_model_t *m, int px, int py, int width, int height)
{
    int x = px * 1000 / width, y = py * m->height / height;
    uint32_t hash = (uint32_t)(px / 2) * 374761393u ^ (uint32_t)(py / 2) * 668265263u ^ m->seed ^ (uint32_t)m->generation * 2246822519u;
    hash = (hash ^ (hash >> 13)) * 1274126177u;
    uint32_t color = 0x050A18;
    if ((hash & 511) < 3) color = (hash & 1024) ? 0x5599B8 : 0xD8EDFF;
    for (int planet = 0; planet < 2; planet++)
    {
        int dx = x - (planet ? 700 : 250), dy = y - (planet ? m->target_y : m->current_y);
        int distance = dx * dx + dy * dy;
        uint32_t base = s_planet_colors[(m->jumps + planet) & 7];
        if (distance < 43 * 43)
        {
            int light = clamp(180 - dx * 2 - dy * 2, 48, 255);
            if (((y + x / 3) / 10 & 1) && planet) light = light * 4 / 5;
            color = shade(base, light);
        }
        else if (distance < 48 * 48) color = shade(base, 70);
        if (distance > 86 * 86 && distance < 93 * 93 && ((x + y) / 12 & 1)) color = shade(base, planet ? 150 : 85);
        if ((m->jumps + planet) % 3 == 2 && distance > 43 * 43)
        {
            int a = dx + dy / 2, b = dy * 4;
            int ring = a * a + b * b;
            if (ring > 62 * 62 && ring < 72 * 72) color = shade(base, 150);
        }
    }
    if (m->jumps >= 3)
    {
        int dx = x - 490, dy = y - m->hole_y;
        int d = dx * dx + dy * dy;
        if (d < 24 * 24) color = 0x01020A;
        else if (d < 31 * 31) color = 0xF978B5;
        else if (d < 42 * 42) color = 0x46245D;
    }
    /* Eight little constellation lights make progress visible without text. */
    if (y > m->height - 24 && y < m->height - 12)
        for (int i = 0; i < Q_GOAL; i++)
            if (abs(x - (360 + i * 40)) < 7) color = i < m->jumps ? 0x9CFFE8 : 0x20324D;
    return mg_color(color);
}
static void build_background(quantum_state_t *s, mg_canvas_t *c)
{
    for (int y = 0; y < (c->height + 1) / 2; y++)
        for (int x = 0; x < (c->width + 1) / 2; x++)
            s->background[y * Q_BACKGROUND_W + x] = space_pixel(&s->model, x * 2, y * 2, c->width, c->height);
    s->cached_generation = s->model.generation;
    s->cached_width = c->width;
    s->cached_height = c->height;
    s->ready = true;
}
static void restore(quantum_state_t *s, mg_canvas_t *c, mg_rect_t r)
{
    r.x1 = clamp(r.x1, 0, c->width - 1);
    r.y1 = clamp(r.y1, 0, c->height - 1);
    r.x2 = clamp(r.x2, 0, c->width - 1);
    r.y2 = clamp(r.y2, 0, c->height - 1);
    for (int y = r.y1; y <= r.y2; y++)
    {
        uint16_t *row = (uint16_t *)(c->pixels + (size_t)y * c->stride);
        const uint16_t *source = s->background + (y / 2) * Q_BACKGROUND_W;
        for (int x = r.x1; x <= r.x2; x++) row[x] = source[x / 2];
    }
    mg_dirty(c, r.x1, r.y1, r.x2, r.y2);
}
static void include(mg_rect_t *r, int x, int y, int radius)
{
    if (x - radius < r->x1) r->x1 = x - radius;
    if (x + radius > r->x2) r->x2 = x + radius;
    if (y - radius < r->y1) r->y1 = y - radius;
    if (y + radius > r->y2) r->y2 = y + radius;
}
static void disc(mg_canvas_t *c, mg_rect_t *bounds, int x, int y, int radius, uint32_t rgb)
{
    uint16_t color = mg_color(rgb);
    for (int py = y - radius; py <= y + radius; py++)
    {
        if (py < 0 || py >= c->height) continue;
        uint16_t *row = (uint16_t *)(c->pixels + (size_t)py * c->stride);
        for (int px = x - radius; px <= x + radius; px++)
            if (px >= 0 && px < c->width && (px - x) * (px - x) + (py - y) * (py - y) <= radius * radius) row[px] = color;
    }
    include(bounds, x, y, radius + 1);
}
static int screen_x(const mg_canvas_t *c, int x) { return x * c->width / 16000; }
static int screen_y(const mg_canvas_t *c, const quantum_model_t *m, int y) { return y * c->height / (16 * m->height); }
static void draw(void *state, mg_canvas_t *c)
{
    quantum_state_t *s = state;
    quantum_model_t *m = &s->model;
    int world_height = c->height * 1000 / c->width;
    if (!s->ready && m->elapsed < 64 && m->height != world_height) init_model(m, m->seed, world_height);
    bool changed = !s->ready || s->cached_generation != m->generation || s->cached_width != c->width || s->cached_height != c->height;
    if (changed)
    {
        build_background(s, c);
        restore(s, c, (mg_rect_t){0, 0, c->width - 1, c->height - 1});
        c->full = true;
    }
    else if (s->previous_valid) restore(s, c, s->previous);
    mg_rect_t bounds = {c->width, c->height, -1, -1};
    for (int i = Q_TRAIL - 1; i >= 0; i--)
        disc(c, &bounds, screen_x(c, m->trail_x[i]), screen_y(c, m, m->trail_y[i]),
             i < 2 ? 2 : 1, i < 2 ? 0x60E8FF : 0x246B9A);
    if (m->orbiting)
    {
        quantum_model_t predicted = *m;
        predicted.charge = m->held ? m->charge : 0;
        launch(&predicted);
        for (int i = 0; i < 5; i++)
        {
            for (int n = 0; n < 9; n++)
            {
                int ax, ay;
                gravity(&predicted, predicted.x / 16, predicted.y / 16, &ax, &ay);
                predicted.vx = clamp(predicted.vx + ax * MG_STEP_MS / 1000, -800, 800);
                predicted.vy = clamp(predicted.vy + ay * MG_STEP_MS / 1000, -800, 800);
                predicted.x += predicted.vx * MG_STEP_MS * 16 / 1000;
                predicted.y += predicted.vy * MG_STEP_MS * 16 / 1000;
            }
            disc(c, &bounds, screen_x(c, predicted.x), screen_y(c, m, predicted.y),
                 i < 2 ? 2 : 1, m->held ? 0xBBFFB7 : 0x52758E);
        }
    }
    for (int i = 0; i < Q_PARTICLES; i++)
        if (m->particles[i].age > 0)
            disc(c, &bounds, screen_x(c, m->particles[i].x), screen_y(c, m, m->particles[i].y),
                 m->particles[i].age > 180 ? 2 : 1, s_planet_colors[(m->jumps + i) & 7]);
    int x = screen_x(c, m->x), y = screen_y(c, m, m->y);
    if (m->held && m->orbiting)
    {
        int length = c->actor_size * m->charge / 100;
        mg_box_pixels(c, x - c->actor_size / 2, y + c->actor_size / 3, c->actor_size, 3, 0x24465B);
        mg_box_pixels(c, x - c->actor_size / 2, y + c->actor_size / 3, length, 3, 0xABFFC6);
    }
    mg_pet_pixels(c, x, y + c->actor_size / 2);
    include(&bounds, x, y, c->actor_size / 2 + 4);
    /* One swept rectangle instead of many tiny panel transfers. */
    if (!c->full)
    {
        c->dirty_count = 0;
        if (s->previous_valid) mg_dirty(c, s->previous.x1, s->previous.y1, s->previous.x2, s->previous.y2);
        mg_dirty(c, bounds.x1, bounds.y1, bounds.x2, bounds.y2);
    }
    s->previous = bounds;
    s->previous_valid = true;
}
static void hud(const void *state, char text[MG_HUD_BYTES])
{
    const quantum_model_t *m = &((const quantum_state_t *)state)->model;
    snprintf(text, MG_HUD_BYTES, "Salti %d/%d  Scudi %d  %s %d", m->jumps, Q_GOAL, m->lives,
             m->held && m->orbiting ? "Carica" : "Energia", m->held && m->orbiting ? m->charge : m->energy);
}
static const char *feedback(const void *state)
{
    const quantum_model_t *m = &((const quantum_state_t *)state)->model;
    if (m->flash > 0)
    {
        if (m->message == 1) return "Salto quantico!";
        if (m->message == 2) return "Rientro d'emergenza!";
        if (m->message == 3) return "Impulso!";
    }
    return m->orbiting ? (m->held ? "Il tempo rallenta. Mira e lascia!" : "Aggancia il prossimo pianeta") : "Tocca per un impulso gravitazionale";
}
static bool result(const void *state, mg_result_t *out)
{
    const quantum_model_t *m = &((const quantum_state_t *)state)->model;
    out->score = m->jumps * 10 + m->perfect * 3;
    out->won = m->jumps >= Q_GOAL;
    return m->done;
}
const mg_game_t mg_quantum = {
    .id = "quantum", .title = "Salto quantico", .instructions = "Tieni e lascia; in volo tocca!",
    .lose_text = "Orbita persa!", .win_text = "Tra le stelle!", .sky = 0x050A18, .ground = 0x081025,
    .state_size = sizeof(quantum_state_t), .start = start, .step = step, .input = input,
    .draw = draw, .hud = hud, .result = result, .feedback = feedback};
