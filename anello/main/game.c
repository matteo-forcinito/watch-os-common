/*
 * Anello - a ring arcade game made for round screens.
 *
 * A ball bounces inside a circle. You move a paddle along the rim (finger
 * around the screen, or the knob) and break the crystal rings in the middle.
 * Clear them all for the next level: faster ball, shorter paddle, tougher
 * crystals. Three balls per game, best score kept in NVS.
 *
 * Physics in screen coordinates, angles clockwise from 3 o'clock (the LVGL arc
 * convention), 4 sub-steps per frame so a fast ball never tunnels through a
 * crystal. Only small things move: the ball and the paddle arc; a broken
 * crystal is hidden once. That keeps 60 fps.
 */
#include "core/app.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/sys.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "nvs.h"
#include "ui/ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define RINGS 3
#define SEGMENTS 16 /* per ring, at most */
#define SUBSTEPS 4
#define LIVES 3
#define PI_F 3.14159265f

#define C_BG 0x05040A
#define C_TRACK 0x1A1830
#define C_PADDLE 0xF5F0FF
#define C_BALL 0xFFFFFF
#define C_MISS 0xFF4D6A

static const uint32_t s_ring_color[RINGS] = {0xFF5FA2, 0xA27BFF, 0x4FD8F0};

typedef enum { READY, PLAYING, LOST, LEVEL_UP, OVER, PAUSED } phase_t;

typedef struct
{
    lv_obj_t *arc;
    float start, end; /* degrees, clockwise from 3 o'clock */
    int hits;         /* hits left: 0 = broken */
} crystal_t;

typedef struct
{
    lv_obj_t *screen, *track, *paddle_arc, *ball, *core, *score_label, *lives, *message, *hint;
    lv_timer_t *timer;
    crystal_t crystals[RINGS][SEGMENTS];
    int segments[RINGS];
    float ring_r[RINGS], ring_w;
    int cx, cy, r; /* paddle radius */
    float bx, by, vx, vy, ball_r, speed;
    float paddle, paddle_target, span; /* degrees */
    bool touching;
    phase_t phase, before_pause;
    int level, lives_left, score, best, chain, left;
    int64_t last_us, phase_until_us;
} game_t;

static game_t *s_game;

/* ------------------------------------------------------------ helpers */

static float wrap180(float degrees)
{
    while (degrees > 180)
        degrees -= 360;
    while (degrees < -180)
        degrees += 360;
    return degrees;
}

static float norm360(float degrees)
{
    while (degrees >= 360)
        degrees -= 360;
    while (degrees < 0)
        degrees += 360;
    return degrees;
}

static int load_best(void)
{
    nvs_handle_t nvs;
    int32_t best = 0;
    if (nvs_open("anello", NVS_READONLY, &nvs) == ESP_OK)
    {
        nvs_get_i32(nvs, "best", &best);
        nvs_close(nvs);
    }
    return best;
}

static void save_best(void *arg)
{
    nvs_handle_t nvs;
    if (nvs_open("anello", NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_i32(nvs, "best", (int32_t)(intptr_t)arg);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static lv_obj_t *arc(lv_obj_t *parent, int cx, int cy, int radius, int width, uint32_t color)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, radius * 2 + width, radius * 2 + width);
    lv_obj_set_pos(a, cx - radius - width / 2, cy - radius - width / 2);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(a, false);
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_rotation(a, 0);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, ui_color(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(a, 0, 0);
    return a;
}

static void set_paddle(game_t *g)
{
    float half = g->span / 2;
    lv_arc_set_angles(g->paddle_arc, (lv_value_precise_t)norm360(g->paddle - half), (lv_value_precise_t)norm360(g->paddle + half));
}

static void show_message(game_t *g, const char *title, const char *hint)
{
    lv_label_set_text(g->message, title);
    lv_label_set_text(g->hint, hint);
    lv_obj_set_hidden(g->message, title[0] == 0);
    lv_obj_set_hidden(g->hint, hint[0] == 0);
}

static void update_hud(game_t *g)
{
    char text[48];
    snprintf(text, sizeof(text), "%d", g->score);
    lv_label_set_text(g->score_label, text);
    char dots[16] = "";
    for (int i = 0; i < LIVES; i++)
        strcat(dots, i < g->lives_left ? "o " : "  ");
    snprintf(text, sizeof(text), "LIV %d   %s", g->level, dots);
    lv_label_set_text(g->lives, text);
}

/* ------------------------------------------------------------ level */

static void build_level(game_t *g)
{
    /* More, smaller, tougher crystals each level. */
    for (int ring = 0; ring < RINGS; ring++)
    {
        int count = 6 + ring * 2 + (g->level - 1) * 2;
        count = count > SEGMENTS ? SEGMENTS : count;
        g->segments[ring] = count;
        float step = 360.0f / count, gap = 5.0f, offset = ring * step / 2 + g->level * 11.0f;
        for (int s = 0; s < SEGMENTS; s++)
        {
            crystal_t *c = &g->crystals[ring][s];
            if (s >= count)
            {
                c->hits = 0;
                lv_obj_set_hidden(c->arc, true);
                continue;
            }
            lv_obj_set_hidden(c->arc, false);
            c->start = norm360(offset + s * step + gap / 2);
            c->end = norm360(offset + (s + 1) * step - gap / 2);
            /* From level 3 the outer ring needs two hits. */
            c->hits = g->level >= 3 && ring == RINGS - 1 ? 2 : 1;
            lv_obj_set_style_arc_opa(c->arc, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(c->arc, ui_color(c->hits == 2 ? 0xFFFFFF : s_ring_color[ring]), LV_PART_INDICATOR);
            lv_arc_set_angles(c->arc, (lv_value_precise_t)c->start, (lv_value_precise_t)c->end);
        }
    }
    g->left = 0;
    for (int ring = 0; ring < RINGS; ring++)
        g->left += g->segments[ring];
    g->span = 58.0f - (g->level - 1) * 4.0f;
    if (g->span < 34)
        g->span = 34;
    g->speed = g->r * (1.05f + 0.12f * (g->level - 1));
    set_paddle(g);
}

static void place_ball(game_t *g)
{
    /* On the paddle's side, heading inward with a little random angle. */
    float a = g->paddle * PI_F / 180.0f;
    g->bx = g->cx + cosf(a) * (g->r - 24);
    g->by = g->cy + sinf(a) * (g->r - 24);
    float jitter = ((int)(esp_random() % 41) - 20) * PI_F / 180.0f;
    g->vx = -cosf(a + jitter) * g->speed;
    g->vy = -sinf(a + jitter) * g->speed;
    lv_obj_set_pos(g->ball, (int32_t)(g->bx - g->ball_r), (int32_t)(g->by - g->ball_r));
}

static void new_game(game_t *g)
{
    g->level = 1;
    g->score = 0;
    g->lives_left = LIVES;
    g->chain = 0;
    build_level(g);
    place_ball(g);
    g->phase = READY;
    show_message(g, "Anello", "Tocca per lanciare");
    update_hud(g);
}

/* ------------------------------------------------------------ physics */

static void hit_crystal(game_t *g, int ring, int s)
{
    crystal_t *c = &g->crystals[ring][s];
    c->hits--;
    g->chain++;
    g->score += 10 * g->chain;
    if (c->hits > 0)
    {
        lv_obj_set_style_arc_color(c->arc, ui_color(s_ring_color[ring]), LV_PART_INDICATOR);
        return;
    }
    lv_obj_set_hidden(c->arc, true);
    g->left--;
}

static void bounce_radial(game_t *g, float ux, float uy)
{
    float dot = g->vx * ux + g->vy * uy;
    g->vx -= 2 * dot * ux;
    g->vy -= 2 * dot * uy;
}

static void step(game_t *g, float dt)
{
    g->bx += g->vx * dt;
    g->by += g->vy * dt;
    float dx = g->bx - g->cx, dy = g->by - g->cy;
    float r = sqrtf(dx * dx + dy * dy);
    if (r < 0.001f)
        return;
    float ux = dx / r, uy = dy / r;
    float theta = norm360(atan2f(dy, dx) * 180.0f / PI_F);
    float outward = g->vx * ux + g->vy * uy;

    /* Core in the middle: a bumper. */
    float core_r = g->r * 0.10f;
    if (r < core_r + g->ball_r && outward < 0)
    {
        bounce_radial(g, ux, uy);
        g->bx = g->cx + ux * (core_r + g->ball_r + 0.5f);
        g->by = g->cy + uy * (core_r + g->ball_r + 0.5f);
        return;
    }

    /* Crystal rings. */
    for (int ring = 0; ring < RINGS; ring++)
    {
        float edge = g->ring_w / 2 + g->ball_r;
        if (fabsf(r - g->ring_r[ring]) > edge)
            continue;
        /* Angular slack: the ball's own size seen from the centre. */
        float slack = g->ball_r / g->ring_r[ring] * 180.0f / PI_F;
        for (int s = 0; s < g->segments[ring]; s++)
        {
            crystal_t *c = &g->crystals[ring][s];
            if (c->hits <= 0)
                continue;
            float from_start = norm360(theta - c->start + slack), length = norm360(c->end - c->start) + 2 * slack;
            if (from_start > length)
                continue;
            hit_crystal(g, ring, s);
            bounce_radial(g, ux, uy);
            /* Step out of the band on the side the ball came from. */
            float out = outward > 0 ? g->ring_r[ring] - edge - 0.5f : g->ring_r[ring] + edge + 0.5f;
            g->bx = g->cx + ux * out;
            g->by = g->cy + uy * out;
            return;
        }
    }

    /* Paddle on the rim. */
    float rim = g->r - 6 - g->ball_r;
    if (r >= rim && outward > 0 && r < rim + 10)
    {
        float diff = wrap180(theta - g->paddle);
        float slack = g->ball_r / g->r * 180.0f / PI_F;
        if (fabsf(diff) <= g->span / 2 + slack)
        {
            /* Where it hits sets the angle: the edges send it sideways (max 55 degrees). */
            float deflect = diff / (g->span / 2) * 55.0f;
            deflect = deflect > 55 ? 55 : deflect < -55 ? -55 : deflect;
            float direction = (theta + 180.0f + deflect) * PI_F / 180.0f;
            g->speed *= 1.012f;
            if (g->speed > g->r * 2.6f)
                g->speed = g->r * 2.6f;
            g->vx = cosf(direction) * g->speed;
            g->vy = sinf(direction) * g->speed;
            g->bx = g->cx + ux * (rim - 0.5f);
            g->by = g->cy + uy * (rim - 0.5f);
            g->chain = 0;
        }
    }
}

/* ------------------------------------------------------------ frame */

static void frame(lv_timer_t *timer)
{
    game_t *g = lv_timer_get_user_data(timer);
    int64_t now = esp_timer_get_time();
    float dt = (float)(now - g->last_us) / 1e6f;
    g->last_us = now;
    if (dt > 0.05f)
        dt = 0.05f; /* after a pause or a slow frame: never jump */
    if (power_screen() == POWER_SCREEN_OFF && g->phase == PLAYING)
    {
        g->before_pause = PLAYING;
        g->phase = PAUSED;
        show_message(g, "Pausa", "Tocca per continuare");
    }

    /* Paddle eases toward the finger: smooth, but never lags more than a frame or two. */
    float delta = wrap180(g->paddle_target - g->paddle);
    if (fabsf(delta) > 0.2f)
    {
        g->paddle = norm360(g->paddle + delta * 0.55f);
        set_paddle(g);
    }

    if (g->phase == READY)
        place_ball(g); /* the ball rides with the paddle until launch */
    else if (g->phase == PLAYING)
    {
        for (int i = 0; i < SUBSTEPS; i++)
            step(g, dt / SUBSTEPS);
        float dx = g->bx - g->cx, dy = g->by - g->cy;
        if (dx * dx + dy * dy > (float)(g->r + 16) * (g->r + 16))
        {
            g->lives_left--;
            g->chain = 0;
            lv_obj_set_style_arc_color(g->track, ui_color(C_MISS), LV_PART_INDICATOR);
            if (g->lives_left <= 0)
            {
                g->phase = OVER;
                char text[64];
                bool record = g->score > g->best;
                if (record)
                {
                    g->best = g->score;
                    sys_post(save_best, (void *)(intptr_t)g->best);
                }
                snprintf(text, sizeof(text), record ? "Nuovo record: %d" : "Record: %d", g->best);
                show_message(g, "Fine partita", text);
            }
            else
            {
                g->phase = LOST;
                g->phase_until_us = now + 900000;
                show_message(g, "", "");
            }
            update_hud(g);
        }
        else if (g->left == 0)
        {
            g->score += 100 * g->level;
            g->level++;
            g->phase = LEVEL_UP;
            g->phase_until_us = now + 1400000;
            char text[32];
            snprintf(text, sizeof(text), "Livello %d", g->level);
            show_message(g, text, "Palla piu veloce, racchetta piu corta");
            update_hud(g);
        }
        lv_obj_set_pos(g->ball, (int32_t)(g->bx - g->ball_r), (int32_t)(g->by - g->ball_r));
    }
    else if ((g->phase == LOST || g->phase == LEVEL_UP) && now > g->phase_until_us)
    {
        lv_obj_set_style_arc_color(g->track, ui_color(C_TRACK), LV_PART_INDICATOR);
        if (g->phase == LEVEL_UP)
            build_level(g);
        place_ball(g);
        g->phase = READY;
        show_message(g, "", "Tocca per lanciare");
    }
}

/* ------------------------------------------------------------ input */

static void aim(game_t *g, lv_indev_t *input)
{
    lv_point_t p;
    lv_indev_get_point(input, &p);
    float dx = (float)(p.x - g->cx), dy = (float)(p.y - g->cy);
    if (dx * dx + dy * dy > 30 * 30) /* ignore the very centre: no direction there */
        g->paddle_target = norm360(atan2f(dy, dx) * 180.0f / PI_F);
}

static void touch(lv_event_t *e)
{
    game_t *g = s_game;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *input = lv_indev_active();
    if (input == NULL)
        return;
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING)
    {
        aim(g, input);
        return;
    }
    if (code != LV_EVENT_CLICKED)
        return;
    if (g->phase == READY)
    {
        g->phase = PLAYING;
        show_message(g, "", "");
    }
    else if (g->phase == PAUSED)
    {
        g->phase = g->before_pause;
        show_message(g, "", g->phase == READY ? "Tocca per lanciare" : "");
    }
    else if (g->phase == OVER)
        new_game(g);
}

static void knob(lv_event_t *e)
{
    lv_port_knob_t *k = lv_event_get_param(e);
    game_t *g = s_game;
    g->paddle_target = norm360(g->paddle_target + k->delta * 10.0f);
    k->handled = true;
}

static void leave(bool confirmed, void *unused)
{
    (void)unused;
    if (confirmed)
        extapp_return_to_launcher();
    else if (s_game->phase == PAUSED)
        show_message(s_game, "Pausa", "Tocca per continuare");
}

/* Back: pause first, then offer to leave. */
static bool back(void)
{
    game_t *g = s_game;
    if (g->phase == PLAYING || g->phase == READY)
    {
        g->before_pause = g->phase;
        g->phase = PAUSED;
        show_message(g, "Pausa", "Tocca per continuare");
        return true;
    }
    ui_confirm("Esci da Anello?", "La partita in corso andra persa.", "Esci", C_MISS, leave, NULL);
    return true;
}

/* ------------------------------------------------------------ scene */

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_color(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    lv_obj_set_clickable(l, false);
    return l;
}

static void scene(game_t *g)
{
    const board_info_t *b = board_info();
    int w = b->width, h = b->height, side = w < h ? w : h;
    g->cx = w / 2;
    g->cy = h / 2;
    g->r = side / 2 - 12;
    g->ball_r = side >= 400 ? 8 : 7;
    g->ring_w = side >= 400 ? 12 : 10;
    for (int ring = 0; ring < RINGS; ring++)
        g->ring_r[ring] = g->r * (0.26f + 0.15f * ring);
    lv_obj_t *s = g->screen;
    lv_obj_set_style_bg_color(s, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s, false);

    g->track = arc(s, g->cx, g->cy, g->r, 2, C_TRACK);
    lv_arc_set_angles(g->track, 0, 360);
    for (int ring = 0; ring < RINGS; ring++)
        for (int i = 0; i < SEGMENTS; i++)
        {
            g->crystals[ring][i].arc = arc(s, g->cx, g->cy, (int)g->ring_r[ring], (int)g->ring_w, s_ring_color[ring]);
            lv_obj_set_style_arc_rounded(g->crystals[ring][i].arc, false, LV_PART_INDICATOR);
            lv_obj_set_hidden(g->crystals[ring][i].arc, true);
        }
    g->core = lv_obj_create(s);
    lv_obj_remove_style_all(g->core);
    int core = (int)(g->r * 0.10f);
    lv_obj_set_size(g->core, core * 2, core * 2);
    lv_obj_set_pos(g->core, g->cx - core, g->cy - core);
    lv_obj_set_style_radius(g->core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g->core, ui_color(0x2A2450), 0);
    lv_obj_set_style_bg_opa(g->core, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g->core, ui_color(0x6D5BD0), 0);
    lv_obj_set_style_border_width(g->core, 2, 0);
    lv_obj_set_clickable(g->core, false);

    g->paddle = g->paddle_target = 90; /* 6 o'clock */
    g->paddle_arc = arc(s, g->cx, g->cy, g->r - 3, 8, C_PADDLE);
    lv_obj_set_style_shadow_width(g->paddle_arc, 0, 0);

    g->ball = lv_obj_create(s);
    lv_obj_remove_style_all(g->ball);
    lv_obj_set_size(g->ball, (int32_t)(g->ball_r * 2), (int32_t)(g->ball_r * 2));
    lv_obj_set_style_radius(g->ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g->ball, ui_color(C_BALL), 0);
    lv_obj_set_style_bg_opa(g->ball, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(g->ball, ui_color(0xB9A8FF), 0);
    lv_obj_set_style_shadow_width(g->ball, 10, 0);
    lv_obj_set_style_shadow_opa(g->ball, 150, 0);
    lv_obj_set_clickable(g->ball, false);

    /* HUD in the free band between the crystals (0.6 r) and the rim, clear of the paddle;
     * messages in the middle on a dark pill, readable over the crystals. */
    g->score_label = text(s, ui_font_title, 0xFFFFFF);
    lv_obj_align(g->score_label, LV_ALIGN_CENTER, 0, -(int)(g->r * 0.80f));
    g->lives = text(s, UI_FONT_SMALL, 0x8E86B8);
    lv_obj_set_style_text_letter_space(g->lives, 2, 0);
    lv_obj_align(g->lives, LV_ALIGN_CENTER, 0, (int)(g->r * 0.72f)); /* inside the waiting ball (0.86 r) */
    g->message = text(s, ui_font_headline, 0xFFFFFF);
    lv_obj_set_style_bg_color(g->message, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(g->message, 215, 0);
    lv_obj_set_style_pad_hor(g->message, 14, 0);
    lv_obj_set_style_pad_ver(g->message, 6, 0);
    lv_obj_set_style_radius(g->message, 14, 0);
    lv_obj_align(g->message, LV_ALIGN_CENTER, 0, -18);
    g->hint = text(s, UI_FONT_SMALL, 0xC9C2EA);
    lv_obj_set_style_bg_color(g->hint, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(g->hint, 215, 0);
    lv_obj_set_style_pad_hor(g->hint, 10, 0);
    lv_obj_set_style_pad_ver(g->hint, 4, 0);
    lv_obj_set_style_radius(g->hint, 10, 0);
    lv_obj_align(g->hint, LV_ALIGN_CENTER, 0, 24);

    lv_obj_set_clickable(s, true);
    lv_obj_add_event_cb(s, touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s, touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s, touch, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s, knob, lv_port_knob_event(), NULL);
}

void anello_start(lv_obj_t *screen)
{
    game_t *g = lv_malloc_zeroed(sizeof(*g));
    ESP_ERROR_CHECK(g == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_game = g;
    g->screen = screen;
    g->best = load_best();
    scene(g);
    new_game(g);
    app_manager_set_home(screen, back);
    g->last_us = esp_timer_get_time();
    g->timer = lv_timer_create(frame, 16, g); /* ~60 fps */
    power_keep_screen_on(true); /* a game in play should not dim; released when the app exits */
}
