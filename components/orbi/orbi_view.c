/* Orbi on screen: see orbi/orbi_view.h and docs/ORBI-GENOME.md ("Drawing"). */
#include "orbi/orbi_view.h"

#include "esp_random.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MS 66

struct orbi_view
{
    orbi_genome_t g;
    int32_t W, H;                /* body size */
    lv_obj_t *root, *rig, *body, *shadow, *mouth;
    lv_obj_t *eye[2], *shine[2], *cheek[2];
    int eyes;                    /* 1 (cyclops) or 2 */
    int32_t eye_x[2], eye_y, eye_w, eye_h;
    int32_t rig_y0, body_x0, body_y0;
    orbi_mood_t mood;
    lv_timer_t *timer;
    uint32_t t;                  /* ms since creation */
    uint32_t next_blink, blink_until;
    int look_x, look_y;          /* -100..100 */
    bool look_fixed;
    uint32_t next_wander;
    uint32_t poke_at;            /* 0 = none */
    int32_t last_float, last_gx, last_gy;
    bool blinking;
};

static lv_color_t hsv(uint16_t h, uint8_t s, uint8_t v)
{
    return lv_color_hsv_to_rgb(h % 360, s, v);
}

/* A plain shape centred at (cx, cy) inside parent. */
static lv_obj_t *shape(lv_obj_t *parent, int32_t cx, int32_t cy, int32_t w, int32_t h, int32_t radius, lv_color_t color, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w < 1 ? 1 : w, h < 1 ? 1 : h);
    lv_obj_set_pos(o, cx - w / 2, cy - h / 2);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    return o;
}

static void place_eyes(orbi_view_t *v, int32_t gx, int32_t gy)
{
    int32_t h = v->blinking || v->mood == ORBI_SLEEP ? (v->eye_h / 8 > 2 ? v->eye_h / 8 : 2) : v->eye_h;
    for (int i = 0; i < v->eyes; i++)
    {
        lv_obj_set_size(v->eye[i], v->eye_w, h);
        lv_obj_set_pos(v->eye[i], v->eye_x[i] - v->eye_w / 2 + gx, v->eye_y - h / 2 + gy);
        lv_obj_set_hidden(v->shine[i], h < v->eye_h / 2);
    }
}

static void apply_mood(orbi_view_t *v)
{
    int32_t W = v->W, line = W * 25 / 1000 > 2 ? W * 25 / 1000 : 2;
    int32_t mw = W * (v->mood == ORBI_LOVE ? 24 : 18) / 100, mh = W * 9 / 100;
    lv_obj_t *m = v->mouth;
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, line, 0);
    lv_obj_set_style_border_color(m, lv_color_hex(0x0A1020), 0);
    lv_obj_set_style_border_opa(m, LV_OPA_COVER, 0);
    switch (v->mood)
    {
    case ORBI_HAPPY:
    case ORBI_LOVE:
        lv_obj_set_style_border_side(m, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_radius(m, mh, 0);
        break;
    case ORBI_SAD:
        lv_obj_set_style_border_side(m, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_radius(m, mh, 0);
        break;
    default:   /* calm, sleepy, sleep: a short line */
        mw = W * 12 / 100;
        mh = line;
        lv_obj_set_style_border_width(m, 0, 0);
        lv_obj_set_style_bg_color(m, lv_color_hex(0x0A1020), 0);
        lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(m, line, 0);
        break;
    }
    lv_obj_set_size(m, mw, mh);
    lv_obj_set_pos(m, W / 2 - mw / 2, v->H * 62 / 100 - mh / 2);
    for (int i = 0; i < 2; i++)
    {
        if (v->cheek[i] != NULL)
        {
            lv_obj_set_style_bg_opa(v->cheek[i], v->mood == ORBI_LOVE ? LV_OPA_90 : LV_OPA_60, 0);
        }
    }
    place_eyes(v, v->last_gx, v->last_gy);
}

static void tick(lv_timer_t *timer)
{
    orbi_view_t *v = lv_timer_get_user_data(timer);
    if (!lv_obj_is_visible(v->root)) return;
    lv_area_t area; lv_obj_get_coords(v->root, &area);
    lv_display_t *display=lv_obj_get_display(v->root);
    if (area.x2<0 || area.y2<0 || area.x1>=lv_display_get_horizontal_resolution(display) || area.y1>=lv_display_get_vertical_resolution(display)) return;
    for (lv_indev_t *input=lv_indev_get_next(NULL); input; input=lv_indev_get_next(input))
        if (lv_indev_get_state(input)==LV_INDEV_STATE_PRESSED) return;
    v->t += FRAME_MS;
    int32_t W = v->W;

    /* Float: 2.5% of the width over 3 s (moves only when a pixel changes). */
    float phase = (float)(v->t % 3000) / 3000.0f * 6.2831853f;
    int32_t fl = v->mood == ORBI_SLEEP ? 0 : (int32_t)(sinf(phase) * W * 0.025f);
    if (fl != v->last_float)
    {
        v->last_float = fl;
        lv_obj_set_y(v->rig, v->rig_y0 + fl);
    }

    /* Poke: squash and spring back in 300 ms. */
    if (v->poke_at != 0)
    {
        uint32_t dt = v->t - v->poke_at;
        float k = dt >= 300 ? 0 : sinf((float)dt / 300.0f * 3.14159265f) * (1.0f - (float)dt / 300.0f);
        int32_t w = (int32_t)(W * (1 + 0.06f * k)), h = (int32_t)(v->H * (1 - 0.10f * k));
        lv_obj_set_size(v->body, w, h);
        lv_obj_set_pos(v->body, v->body_x0 - (w - W) / 2, v->body_y0 + (v->H - h));
        if (dt >= 300)
        {
            v->poke_at = 0;
        }
    }

    /* Blink every 3-6 s, 120 ms. */
    bool blink = v->t >= v->next_blink && v->t < v->next_blink + 120;
    if (v->t >= v->next_blink + 120)
    {
        v->next_blink = v->t + 3000 + esp_random() % 3000;
    }
    /* Gaze: wander to a new point every 1.5-4 s unless told where to look. */
    if (!v->look_fixed && v->t >= v->next_wander)
    {
        v->look_x = (int)(esp_random() % 161) - 80;
        v->look_y = (int)(esp_random() % 101) - 50;
        v->next_wander = v->t + 1500 + esp_random() % 2500;
    }
    int32_t reach = v->eye_h * 12 / 100;
    int32_t gx = v->look_x * reach / 100, gy = v->look_y * reach / 100;
    if (blink != v->blinking || gx != v->last_gx || gy != v->last_gy)
    {
        v->blinking = blink;
        v->last_gx = gx;
        v->last_gy = gy;
        place_eyes(v, gx, gy);
    }
}

static void on_delete(lv_event_t *e)
{
    orbi_view_t *v = lv_event_get_user_data(e);
    lv_timer_delete(v->timer);
    free(v);
}

orbi_view_t *orbi_view_create(lv_obj_t *parent, const orbi_genome_t *g, int32_t W)
{
    orbi_view_t *v = calloc(1, sizeof(*v));
    if (v == NULL)
    {
        return NULL;
    }
    v->g = *g;
    v->W = W;
    v->H = g->body == ORBI_BODY_CAPSULE ? W * 125 * g->stretch / 10000 : W * g->stretch / 100;
    int32_t H = v->H;
    lv_color_t body = hsv(g->hue, 55, 95), belly = hsv(g->hue, 25, 100), accent = hsv(g->hue2, 60, 90);
    lv_color_t ink = lv_color_hex(0x0A1020), green = hsv(120, 60, 80);

    int32_t cw = W * 150 / 100, top = W * 35 / 100, ch = top + H + W * 30 / 100;
    v->root = lv_obj_create(parent);
    lv_obj_remove_style_all(v->root);
    lv_obj_set_size(v->root, cw, ch);
    lv_obj_set_clickable(v->root, false);
    lv_obj_set_scrollable(v->root, false);

    /* Shadow on the ground, outside the floating rig. */
    v->shadow = shape(v->root, cw / 2, top + H + W * 6 / 100, W * 70 / 100, W * 8 / 100, LV_RADIUS_CIRCLE, lv_color_black(), LV_OPA_40);

    v->rig = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->rig);
    lv_obj_set_size(v->rig, cw, top + H);
    lv_obj_set_clickable(v->rig, false);
    lv_obj_set_scrollable(v->rig, false);
    v->rig_y0 = 0;
    int32_t bx = (cw - W) / 2, by = top;
    v->body_x0 = bx;
    v->body_y0 = by;

    /* Behind the body: ears and crest (coordinates relative to the body origin). */
    switch (g->ear)
    {
    case ORBI_EAR_ROUND:
        for (int s = 0; s < 2; s++)
        {
            int32_t ex = bx + W * (s ? 82 : 18) / 100, ey = by + H * 2 / 100;
            shape(v->rig, ex, ey, W * 30 / 100, W * 30 / 100, LV_RADIUS_CIRCLE, body, LV_OPA_COVER);
            shape(v->rig, ex, ey, W * 16 / 100, W * 16 / 100, LV_RADIUS_CIRCLE, accent, LV_OPA_COVER);
        }
        break;
    case ORBI_EAR_POINTY:
        for (int s = 0; s < 2; s++)
        {
            shape(v->rig, bx + W * (s ? 78 : 22) / 100, by, W * 16 / 100, W * 34 / 100, W * 8 / 100, body, LV_OPA_COVER);
        }
        break;
    case ORBI_EAR_NUBS:
        for (int s = 0; s < 2; s++)
        {
            shape(v->rig, bx + W * (s ? 75 : 25) / 100, by + H * 4 / 100, W * 14 / 100, W * 14 / 100, LV_RADIUS_CIRCLE, body, LV_OPA_COVER);
        }
        break;
    default:
        break;
    }
    int32_t cx = bx + W / 2;
    switch (g->crest)
    {
    case ORBI_CREST_SPROUT:
        shape(v->rig, cx, by - W * 8 / 100, W * 3 / 100, W * 16 / 100, W, green, LV_OPA_COVER);
        shape(v->rig, cx + W * 7 / 100, by - W * 14 / 100, W * 14 / 100, W * 8 / 100, LV_RADIUS_CIRCLE, green, LV_OPA_COVER);
        break;
    case ORBI_CREST_HORN:
        shape(v->rig, cx, by - W * 10 / 100, W * 8 / 100, W * 20 / 100, W * 4 / 100, accent, LV_OPA_COVER);
        break;
    case ORBI_CREST_ANTENNA:
        shape(v->rig, cx, by - W * 11 / 100, W * 25 / 1000 + 1, W * 22 / 100, W, ink, LV_OPA_80);
        shape(v->rig, cx, by - W * 22 / 100, W * 10 / 100, W * 10 / 100, LV_RADIUS_CIRCLE, accent, LV_OPA_COVER);
        break;
    case ORBI_CREST_NOTE:
        shape(v->rig, cx + W * 5 / 100, by - W * 10 / 100, W * 3 / 100, W * 20 / 100, W, accent, LV_OPA_COVER);
        shape(v->rig, cx + W * 1 / 100, by - W * 1 / 100, W * 11 / 100, W * 9 / 100, LV_RADIUS_CIRCLE, accent, LV_OPA_COVER);
        break;
    default:
        break;
    }

    /* The body and what is on it. */
    int32_t radius = g->body == ORBI_BODY_CUBE ? W * 28 / 100 : (W < H ? W : H) / 2;
    v->body = shape(v->rig, bx + W / 2, by + H / 2, W, H, radius, body, LV_OPA_COVER);
    switch (g->pattern)
    {
    case ORBI_PATTERN_BELLY:
        shape(v->body, W / 2, H * 70 / 100, W * 56 / 100, H * 42 / 100, LV_RADIUS_CIRCLE, belly, LV_OPA_COVER);
        break;
    case ORBI_PATTERN_SPOTS:
    {
        static const uint8_t spots[4][2] = {{22, 30}, {78, 26}, {70, 72}, {30, 78}};
        for (int i = 0; i < 4; i++)
        {
            shape(v->body, W * spots[i][0] / 100, H * spots[i][1] / 100, W / 10, W / 10, LV_RADIUS_CIRCLE, accent, LV_OPA_COVER);
        }
        break;
    }
    case ORBI_PATTERN_STRIPES:
        for (int i = 0; i < 3; i++)
        {
            shape(v->body, W / 2, H * (10 + 8 * i) / 100, W * 36 / 100, W * 5 / 100, W, accent, LV_OPA_COVER);
        }
        break;
    default:
        break;
    }

    int32_t e = W * g->eye_size / 100;
    v->eyes = g->eyes == ORBI_EYES_CYCLOPS ? 1 : 2;
    v->eye_y = H * 42 / 100;
    switch (g->eyes)
    {
    case ORBI_EYES_OVAL: v->eye_w = e * 8 / 10; v->eye_h = e * 125 / 100; break;
    case ORBI_EYES_WIDE: v->eye_w = e * 13 / 10; v->eye_h = e * 9 / 10; break;
    case ORBI_EYES_SLEEPY: v->eye_w = e; v->eye_h = e * 55 / 100; break;
    case ORBI_EYES_SPARKLE: v->eye_w = e * 11 / 10; v->eye_h = e * 11 / 10; break;
    case ORBI_EYES_CYCLOPS: v->eye_w = e * 2; v->eye_h = e * 2; break;
    default: v->eye_w = e; v->eye_h = e; break;
    }
    int32_t half_gap = W * g->eye_gap / 200;
    v->eye_x[0] = v->eyes == 1 ? W / 2 : W / 2 - half_gap;
    v->eye_x[1] = W / 2 + half_gap;
    for (int i = 0; i < v->eyes; i++)
    {
        v->eye[i] = shape(v->body, v->eye_x[i], v->eye_y, v->eye_w, v->eye_h, LV_RADIUS_CIRCLE, ink, LV_OPA_COVER);
        int32_t s = v->eye_w * 30 / 100;
        v->shine[i] = shape(v->eye[i], v->eye_w / 2 + v->eye_w * 15 / 100, v->eye_h / 2 - v->eye_h * 18 / 100, s, s, LV_RADIUS_CIRCLE, lv_color_white(), LV_OPA_COVER);
        if (g->eyes == ORBI_EYES_SPARKLE)
        {
            shape(v->eye[i], v->eye_w / 2 - v->eye_w * 15 / 100, v->eye_h / 2 + v->eye_h * 18 / 100, s / 2, s / 2, LV_RADIUS_CIRCLE, lv_color_white(), LV_OPA_COVER);
        }
    }
    if (g->pattern == ORBI_PATTERN_FRECKLES)
    {
        for (int i = 0; i < v->eyes; i++)
        {
            for (int k = -1; k <= 1; k++)
            {
                shape(v->body, v->eye_x[i] + k * W * 5 / 100, v->eye_y + W * 13 / 100 + (k == 0 ? W / 50 : 0), W * 35 / 1000 + 1, W * 35 / 1000 + 1, LV_RADIUS_CIRCLE, accent, LV_OPA_COVER);
            }
        }
    }
    if (g->cheeks)
    {
        for (int i = 0; i < 2; i++)
        {
            int32_t x = v->eyes == 1 ? (i ? W * 70 / 100 : W * 30 / 100) : v->eye_x[i] + (i ? W * 6 / 100 : -W * 6 / 100);
            v->cheek[i] = shape(v->body, x, v->eye_y + W * 13 / 100, W * 14 / 100, W * 8 / 100, LV_RADIUS_CIRCLE, hsv(350, 45, 100), LV_OPA_60);
        }
    }
    v->mouth = shape(v->body, W / 2, H * 62 / 100, W * 18 / 100, W * 9 / 100, 0, ink, LV_OPA_TRANSP);

    v->mood = ORBI_HAPPY;
    v->next_blink = 2000 + esp_random() % 2000;
    v->last_float = 1 << 30;
    apply_mood(v);
    v->timer = lv_timer_create(tick, FRAME_MS, v);
    lv_obj_add_event_cb(v->root, on_delete, LV_EVENT_DELETE, v);
    return v;
}

lv_obj_t *orbi_view_root(orbi_view_t *v)
{
    return v->root;
}

void orbi_view_mood(orbi_view_t *v, orbi_mood_t mood)
{
    if (v->mood != mood)
    {
        v->mood = mood;
        apply_mood(v);
    }
}

void orbi_view_look(orbi_view_t *v, int x, int y)
{
    v->look_fixed = true;
    v->look_x = x < -100 ? -100 : x > 100 ? 100 : x;
    v->look_y = y < -100 ? -100 : y > 100 ? 100 : y;
}

void orbi_view_look_free(orbi_view_t *v)
{
    v->look_fixed = false;
}

void orbi_view_poke(orbi_view_t *v)
{
    v->poke_at = v->t == 0 ? 1 : v->t;
}

void orbi_view_delete(orbi_view_t *v)
{
    lv_obj_delete(v->root);   /* frees v through on_delete */
}
void orbi_view_static(orbi_view_t *v) { lv_timer_pause(v->timer); }
