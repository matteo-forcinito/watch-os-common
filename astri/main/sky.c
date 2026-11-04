/* Astri: the catalogue and the sky renderer. See astri.h. */
#include "astri.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui/ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "astri_sky";

#define DEG (float)(M_PI / 180.0)
#define PICK_MAX 160

extern const uint8_t stars_bin_start[] asm("_binary_stars_bin_start");
extern const uint8_t lines_bin_start[] asm("_binary_lines_bin_start");
extern const uint8_t labels_bin_start[] asm("_binary_labels_bin_start");
extern const uint8_t mw_bin_start[] asm("_binary_mw_bin_start");

typedef struct
{
    float v[3];        /* unit vector, equatorial J2000 */
    int8_t mag10;
    uint16_t color;    /* RGB565 from B-V */
} star_t;

typedef struct
{
    float a[3], b[3];
} segment_t;

typedef struct
{
    float v[3];
    uint8_t kind;      /* 0 constellation, 1 star, 2 deep sky */
    int8_t mag10;
    char text[24];
} label_t;

typedef struct
{
    int16_t x, y;
    char name[24];
} pick_t;

static star_t *s_stars;
static int s_star_count;
static segment_t *s_segments;
static int s_segment_count;
static label_t *s_labels;
static int s_label_count;
static const uint8_t *s_mw;
static int s_mw_w, s_mw_h;
static pick_t *s_picks;
static int s_pick_count;
static float s_sun_alt = -90.0f;

/* ------------------------------------------------------------- catalogue */

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void unit(uint16_t ra_q, int16_t dec_q, float v[3])
{
    float ra = ra_q * (360.0f / 65536.0f) * DEG, dec = dec_q * (90.0f / 32767.0f) * DEG;
    v[0] = cosf(dec) * cosf(ra);
    v[1] = cosf(dec) * sinf(ra);
    v[2] = sinf(dec);
}

static uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Star colour from the B-V index (blue-white hot stars to orange cool ones). */
static uint16_t bv_color(uint8_t index)
{
    static const uint8_t ramp[][3] = {{155, 176, 255}, {202, 215, 255}, {248, 247, 255}, {255, 244, 234},
                                      {255, 222, 180}, {255, 204, 130}, {255, 180, 95}};
    float bv = index / 255.0f * 2.4f - 0.4f;
    float f = (bv + 0.4f) / 0.4f;   /* one ramp step per 0.4 of B-V */
    if (f < 0) f = 0;
    if (f > 6) f = 6;
    int i = (int)f;
    float t = f - i;
    int j = i < 6 ? i + 1 : 6;
    return rgb565((int)(ramp[i][0] + (ramp[j][0] - ramp[i][0]) * t), (int)(ramp[i][1] + (ramp[j][1] - ramp[i][1]) * t),
                  (int)(ramp[i][2] + (ramp[j][2] - ramp[i][2]) * t));
}

bool astri_sky_load(void)
{
    if (memcmp(stars_bin_start, "AST1", 4) || memcmp(lines_bin_start, "ASL1", 4) ||
        memcmp(labels_bin_start, "ASN1", 4) || memcmp(mw_bin_start, "ASM1", 4))
    {
        ESP_LOGE(TAG, "Sky assets missing or of another version");
        return false;
    }

    s_star_count = (int)u32(stars_bin_start + 4);
    s_segment_count = (int)u32(lines_bin_start + 4);
    s_label_count = (int)u32(labels_bin_start + 4);
    s_stars = heap_caps_calloc(s_star_count, sizeof(star_t), MALLOC_CAP_SPIRAM);
    s_segments = heap_caps_calloc(s_segment_count, sizeof(segment_t), MALLOC_CAP_SPIRAM);
    s_labels = heap_caps_calloc(s_label_count, sizeof(label_t), MALLOC_CAP_SPIRAM);
    s_picks = heap_caps_calloc(PICK_MAX, sizeof(pick_t), MALLOC_CAP_SPIRAM);
    if (!s_stars || !s_segments || !s_labels || !s_picks)
    {
        return false;
    }

    const uint8_t *p = stars_bin_start + 8;
    for (int i = 0; i < s_star_count; i++, p += 6)
    {
        unit(u16(p), (int16_t)u16(p + 2), s_stars[i].v);
        s_stars[i].mag10 = (int8_t)p[4];
        s_stars[i].color = bv_color(p[5]);
    }

    p = lines_bin_start + 8;
    for (int i = 0; i < s_segment_count; i++, p += 8)
    {
        unit(u16(p), (int16_t)u16(p + 2), s_segments[i].a);
        unit(u16(p + 4), (int16_t)u16(p + 6), s_segments[i].b);
    }

    p = labels_bin_start + 8;
    for (int i = 0; i < s_label_count; i++)
    {
        unit(u16(p), (int16_t)u16(p + 2), s_labels[i].v);
        s_labels[i].kind = p[4];
        s_labels[i].mag10 = (int8_t)p[5];
        int len = p[6] < sizeof(s_labels[i].text) - 1 ? p[6] : (int)sizeof(s_labels[i].text) - 1;
        memcpy(s_labels[i].text, p + 7, len);
        p += 7 + p[6];
    }

    s_mw_w = u16(mw_bin_start + 4);
    s_mw_h = u16(mw_bin_start + 6);
    s_mw = mw_bin_start + 8;
    ESP_LOGI(TAG, "%d stars, %d lines, %d labels, Milky Way %dx%d", s_star_count, s_segment_count, s_label_count,
             s_mw_w, s_mw_h);
    return true;
}

/* -------------------------------------------------------------- drawing */

typedef struct
{
    const astri_view_t *view;
    astri_frame_t frame;
} ctx_t;

static void rotate(const astri_frame_t *f, const float v[3], float enu[3])
{
    for (int i = 0; i < 3; i++)
    {
        enu[i] = f->m[i][0] * v[0] + f->m[i][1] * v[1] + f->m[i][2] * v[2];
    }
}

/* Stereographic: zenith at the centre, horizon on the edge, east on the left. */
static bool project(const ctx_t *c, const float enu[3], float *x, float *y)
{
    float k = 1.0f + enu[2];
    if (k < 0.05f)
    {
        return false;
    }
    *x = c->view->cx - c->view->radius * enu[0] / k;
    *y = c->view->cy - c->view->radius * enu[1] / k;
    return true;
}

static void add_pixel(const astri_view_t *v, int x, int y, uint16_t color, int alpha)
{
    if (x < 0 || y < 0 || x >= v->width || y >= v->height)
    {
        return;
    }
    int dx = x - v->cx, dy = y - v->cy;
    if (dx * dx + dy * dy > v->radius * v->radius)
    {
        return;
    }
    uint16_t *p = &v->pixels[y * v->width + x];
    int r = ((*p >> 11) & 31) + (((color >> 11) & 31) * alpha >> 8);
    int g = ((*p >> 5) & 63) + (((color >> 5) & 63) * alpha >> 8);
    int b = (*p & 31) + ((color & 31) * alpha >> 8);
    *p = (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
}

/* A star: one pixel for the faint ones, a soft dot for the bright ones. */
static void plot_star(const astri_view_t *v, float fx, float fy, uint16_t color, float mag, float fade)
{
    int x = (int)(fx + 0.5f), y = (int)(fy + 0.5f);
    float level = (6.2f - mag) / 5.0f;   /* mag 6.2 -> 0, mag 1.2 -> 1 */
    if (level <= 0)
    {
        return;
    }
    int alpha = (int)(fminf(level, 1.0f) * 255 * fade);
    add_pixel(v, x, y, color, alpha);
    if (mag < 3.4f)
    {
        int side = (int)(alpha * fminf((3.4f - mag) / 2.4f, 1.0f) * 0.7f);
        add_pixel(v, x + 1, y, color, side);
        add_pixel(v, x - 1, y, color, side);
        add_pixel(v, x, y + 1, color, side);
        add_pixel(v, x, y - 1, color, side);
    }
    if (mag < 1.2f)
    {
        int halo = (int)(alpha * 0.35f);
        for (int dy = -2; dy <= 2; dy++)
        {
            for (int dx = -2; dx <= 2; dx++)
            {
                int d2 = dx * dx + dy * dy;
                if (d2 >= 2 && d2 <= 5)
                {
                    add_pixel(v, x + dx, y + dy, color, halo);
                }
            }
        }
    }
}

static void disc(const astri_view_t *v, float fx, float fy, int r, uint16_t color)
{
    for (int dy = -r; dy <= r; dy++)
    {
        for (int dx = -r; dx <= r; dx++)
        {
            if (dx * dx + dy * dy <= r * r)
            {
                add_pixel(v, (int)fx + dx, (int)fy + dy, color, 255);
            }
        }
    }
}

/* The Moon with its lit part: the terminator is an ellipse across the disc. */
static void moon_disc(const astri_view_t *v, float fx, float fy, int r, float lit, bool waxing)
{
    float k = 1.0f - 2.0f * lit;   /* terminator x at each row: k * half-width */
    for (int dy = -r; dy <= r; dy++)
    {
        float half = sqrtf((float)(r * r - dy * dy));
        for (int dx = -r; dx <= r; dx++)
        {
            if (dx * dx + dy * dy > r * r)
            {
                continue;
            }
            /* Seen from the northern sky with east on the left: the waxing Moon is lit on the west (right). */
            float side = waxing ? (float)dx : (float)-dx;
            bool on = side >= k * half;
            add_pixel(v, (int)fx + dx, (int)fy + dy, on ? rgb565(235, 232, 215) : rgb565(40, 42, 50), 255);
        }
    }
}

/* atan2 accurate to ~0.3 degrees: plenty for a 1024 px wide texture. */
static float fast_atan2(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y);
    float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (mx < 1e-9f)
    {
        return 0.0f;
    }
    float a = mn / mx, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

static uint16_t sky_background(float sun_alt)
{
    /* Night below -18 deg, astronomical to civil twilight blue, then day. */
    if (sun_alt < -18.0f) return rgb565(0, 2, 8);
    if (sun_alt < -6.0f)
    {
        float t = (sun_alt + 18.0f) / 12.0f;
        return rgb565((int)(4 + 10 * t), (int)(6 + 26 * t), (int)(18 + 50 * t));
    }
    if (sun_alt < 0.0f)
    {
        float t = (sun_alt + 6.0f) / 6.0f;
        return rgb565((int)(14 + 40 * t), (int)(32 + 70 * t), (int)(68 + 90 * t));
    }
    return rgb565(56, 108, 170);
}

static void milky_way(const ctx_t *c, float strength)
{
    const astri_view_t *v = c->view;
    const astri_frame_t *f = &c->frame;
    uint16_t tint = rgb565(170, 185, 230);

    for (int y = v->cy - v->radius; y < v->cy + v->radius; y += 2)
    {
        for (int x = v->cx - v->radius; x < v->cx + v->radius; x += 2)
        {
            float px = (float)(x + 1 - v->cx) / v->radius, py = (float)(v->cy - y - 1) / v->radius;
            float s2 = px * px + py * py;
            if (s2 > 1.0f)
            {
                continue;
            }
            float k = 2.0f / (1.0f + s2);
            float enu[3] = {-px * k, py * k, (1.0f - s2) / (1.0f + s2)};
            /* Back to equatorial: the frame is a rotation, its transpose inverts it. */
            float ex = f->m[0][0] * enu[0] + f->m[1][0] * enu[1] + f->m[2][0] * enu[2];
            float ey = f->m[0][1] * enu[0] + f->m[1][1] * enu[1] + f->m[2][1] * enu[2];
            float ez = f->m[0][2] * enu[0] + f->m[1][2] * enu[1] + f->m[2][2] * enu[2];
            float ra = fast_atan2(ey, ex);
            float dec = fast_atan2(ez, sqrtf(ex * ex + ey * ey));
            int tx = (int)((ra < 0 ? ra + 6.2831853f : ra) * (s_mw_w / 6.2831853f)) % s_mw_w;
            int ty = (int)((1.5707963f - dec) * (s_mw_h / 3.1415927f));
            if (ty < 0) ty = 0;
            if (ty >= s_mw_h) ty = s_mw_h - 1;
            int level = (int)(s_mw[ty * s_mw_w + tx] * strength);
            if (level > 2)
            {
                add_pixel(v, x, y, tint, level);
                add_pixel(v, x + 1, y, tint, level);
                add_pixel(v, x, y + 1, tint, level);
                add_pixel(v, x + 1, y + 1, tint, level);
            }
        }
    }
}

static void remember(int x, int y, const char *name)
{
    if (s_pick_count < PICK_MAX)
    {
        s_picks[s_pick_count].x = (int16_t)x;
        s_picks[s_pick_count].y = (int16_t)y;
        snprintf(s_picks[s_pick_count].name, sizeof(s_picks[0].name), "%s", name);
        s_pick_count++;
    }
}

/* Names already written in this frame: a new one that would overlap is skipped,
 * so the order of the calls is the priority (planets, bright stars, figures). */
#define PLACED_MAX 96
static lv_area_t s_placed[PLACED_MAX];
static int s_placed_count;

static bool label(lv_layer_t *layer, int x, int y, const char *text, lv_color_t color, const lv_font_t *font)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_area_t box = {x - size.x / 2 - 2, y - 1, x + size.x / 2 + 2, y + size.y};
    for (int i = 0; i < s_placed_count; i++)
    {
        if (box.x1 <= s_placed[i].x2 && box.x2 >= s_placed[i].x1 && box.y1 <= s_placed[i].y2 && box.y2 >= s_placed[i].y1)
        {
            return false;
        }
    }
    if (s_placed_count < PLACED_MAX)
    {
        s_placed[s_placed_count++] = box;
    }
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.font = font;
    d.color = color;
    d.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t area = {x - 70, y, x + 70, y + size.y};
    lv_draw_label(layer, &d, &area);
    return true;
}

void astri_sky_render(const astri_view_t *view, void *lv_canvas, time_t t, float lat, float lon, bool fast)
{
    lv_obj_t *canvas = lv_canvas;
    ctx_t c = {.view = view};
    astri_frame(&c.frame, t, lat, lon);
    s_pick_count = 0;

    astri_position_t bodies[ASTRI_BODIES];
    for (int b = 0; b < ASTRI_BODIES; b++)
    {
        astri_body((astri_body_t)b, t, &bodies[b]);
    }
    float sun_enu[3];
    astri_to_horizontal(&c.frame, bodies[ASTRI_SUN].ra, bodies[ASTRI_SUN].dec, sun_enu);
    s_sun_alt = asinf(sun_enu[2]) / DEG;

    /* Stars fade out through twilight; by day only the Moon, Venus and Jupiter remain. */
    float night = s_sun_alt < -18.0f ? 1.0f : s_sun_alt > -4.0f ? 0.0f : (-4.0f - s_sun_alt) / 14.0f;

    /* Background: the sky disc in its colour, black outside. */
    uint16_t bg = sky_background(s_sun_alt);
    int r2 = view->radius * view->radius;
    for (int y = 0; y < view->height; y++)
    {
        uint16_t *row = &view->pixels[y * view->width];
        int dy = y - view->cy;
        for (int x = 0; x < view->width; x++)
        {
            int dx = x - view->cx;
            row[x] = dx * dx + dy * dy <= r2 ? bg : 0;
        }
    }

    if (!fast && night > 0.05f)
    {
        milky_way(&c, 0.55f * night);
    }

    /* Constellation figures (under the stars). */
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    if (night > 0.05f)
    {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = lv_color_make(40, 70, 120);
        line.opa = (lv_opa_t)(200 * night);
        line.width = 1;
        for (int i = 0; i < s_segment_count; i++)
        {
            float a[3], b[3], ax, ay, bx, by;
            rotate(&c.frame, s_segments[i].a, a);
            rotate(&c.frame, s_segments[i].b, b);
            if (a[2] < -0.02f || b[2] < -0.02f || !project(&c, a, &ax, &ay) || !project(&c, b, &bx, &by))
            {
                continue;
            }
            line.p1.x = ax;
            line.p1.y = ay;
            line.p2.x = bx;
            line.p2.y = by;
            lv_draw_line(&layer, &line);
        }
    }
    lv_canvas_finish_layer(canvas, &layer);

    /* Stars. */
    for (int i = 0; i < s_star_count && night > 0.0f; i++)
    {
        float e[3], x, y;
        rotate(&c.frame, s_stars[i].v, e);
        if (e[2] > 0.0f && project(&c, e, &x, &y))
        {
            plot_star(view, x, y, s_stars[i].color, s_stars[i].mag10 / 10.0f, night);
        }
    }

    /* Sun, Moon, planets. */
    float lit;
    bool waxing;
    astri_moon_phase(t, &lit, &waxing);
    static const uint8_t planet_rgb[ASTRI_BODIES][3] = {
        [ASTRI_MERCURY] = {200, 190, 170}, [ASTRI_VENUS] = {255, 250, 225}, [ASTRI_MARS] = {255, 120, 80},
        [ASTRI_JUPITER] = {250, 225, 180}, [ASTRI_SATURN] = {235, 210, 140}};
    float bx[ASTRI_BODIES], by[ASTRI_BODIES];
    bool shown[ASTRI_BODIES] = {0};
    for (int b = 0; b < ASTRI_BODIES; b++)
    {
        float e[3];
        astri_to_horizontal(&c.frame, bodies[b].ra, bodies[b].dec, e);
        if (e[2] <= 0.0f || !project(&c, e, &bx[b], &by[b]))
        {
            continue;
        }
        shown[b] = true;
        if (b == ASTRI_SUN) disc(view, bx[b], by[b], 7, rgb565(255, 225, 120));
        else if (b == ASTRI_MOON) moon_disc(view, bx[b], by[b], 7, lit, waxing);
        else if (night > 0.0f || b == ASTRI_VENUS || b == ASTRI_JUPITER)
            disc(view, bx[b], by[b], bodies[b].mag < -1.0f ? 3 : 2,
                 rgb565(planet_rgb[b][0], planet_rgb[b][1], planet_rgb[b][2]));
        remember((int)bx[b], (int)by[b], astri_body_name((astri_body_t)b));
    }

    /* Names, by priority: date line and cardinal points are reserved first,
     * then planets, the brightest stars, the constellations. */
    lv_canvas_init_layer(canvas, &layer);
    s_placed_count = 0;
    static const char *const cardinal[] = {"N", "E", "S", "O"};
    int r = view->radius - 16;
    int pos[4][2] = {{view->cx, view->cy - r}, {view->cx - r, view->cy}, {view->cx, view->cy + r}, {view->cx + r, view->cy}};
    for (int i = 0; i < 4; i++)
    {
        label(&layer, pos[i][0], pos[i][1] - 4, cardinal[i], i == 0 ? lv_color_make(255, 110, 90) : lv_color_make(140, 160, 190),
              UI_FONT_HUD);
    }
    /* The date and the time offset at the bottom. */
    s_placed[s_placed_count++] = (lv_area_t){view->cx - 90, view->cy + view->radius - 80, view->cx + 90, view->cy + view->radius - 26};
    static const astri_body_t by_brightness[] = {ASTRI_MOON, ASTRI_VENUS, ASTRI_JUPITER, ASTRI_MARS, ASTRI_SATURN,
                                                 ASTRI_MERCURY};
    for (int k = 0; k < (int)(sizeof(by_brightness) / sizeof(by_brightness[0])); k++)
    {
        astri_body_t b = by_brightness[k];
        if (shown[b])
        {
            label(&layer, (int)bx[b], (int)by[b] + 8, astri_body_name((astri_body_t)b), lv_color_make(240, 200, 120),
                  UI_FONT_MICRO);
        }
    }
    for (int pass = 1; pass >= 0; pass--)   /* stars (kind 1) before figures (kind 0) */
    {
        for (int i = 0; i < s_label_count; i++)
        {
            if (s_labels[i].kind != pass && !(pass == 1 && s_labels[i].kind == 2))
            {
                continue;
            }
            float e[3], x, y;
            rotate(&c.frame, s_labels[i].v, e);
            if (e[2] < 0.12f || !project(&c, e, &x, &y))
            {
                continue;
            }
            remember((int)x, (int)y, s_labels[i].text);
            if (night < 0.3f || fast)
            {
                continue;   /* names only in a dark sky, and not while the sky turns */
            }
            if (s_labels[i].kind == 0)
                label(&layer, (int)x, (int)y - 6, s_labels[i].text, lv_color_make(70, 100, 150), UI_FONT_MICRO);
            else if (s_labels[i].kind == 1 && s_labels[i].mag10 <= 12)
                label(&layer, (int)x, (int)y + 5, s_labels[i].text, lv_color_make(150, 160, 185), UI_FONT_MICRO);
        }
    }

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x = view->cx;
    arc.center.y = view->cy;
    arc.radius = (uint16_t)view->radius;
    arc.start_angle = 0;
    arc.end_angle = 360;
    arc.width = 2;
    arc.color = lv_color_make(60, 80, 110);
    lv_draw_arc(&layer, &arc);

    lv_canvas_finish_layer(canvas, &layer);
}

const char *astri_sky_pick(int x, int y, int max_px, char *buffer, int size)
{
    int best = -1, best_d2 = max_px * max_px;
    for (int i = 0; i < s_pick_count; i++)
    {
        int dx = s_picks[i].x - x, dy = s_picks[i].y - y, d2 = dx * dx + dy * dy;
        if (d2 <= best_d2)
        {
            best = i;
            best_d2 = d2;
        }
    }
    if (best < 0)
    {
        return NULL;
    }
    snprintf(buffer, size, "%s", s_picks[best].name);
    return buffer;
}

float astri_sky_sun_altitude(void)
{
    return s_sun_alt;
}
