/*
 * Orbit Link scenes: short animations drawn above every app on lv_layer_top(),
 * driven by the companion so that several devices act together.
 *
 *   pulse   a ring of light that grows and fades (the "wave" across devices)
 *   aura    the screen border breathes in one colour
 *   beam    a comet leaves towards `angle` (phase out) or arrives from it (in)
 *   text    a holographic message; a tap dismisses it
 *   light   the whole screen in one colour (flashlight); a tap switches it off
 *   orbi    Orbi, the ecosystem's creature: touching it is reported to Orbit
 *   dial    modal remote: knob or finger turn a ring, steps go to Orbit
 *   clear   removes the current scene
 *
 * One scene at a time. Scenes that are not modal let touches through to the
 * app underneath. Everything here runs in the lvgl task.
 */
#include "link_internal.h"

#include "core/lv_port.h"
#include "core/power.h"
#include "hardware/board.h"
#include "ui/ui.h"
#include "orbi/orbi_view.h"

#include "esp_heap_caps.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEG_TO_RAD 0.01745329252f
#define COMET      34    /* beam head, px */

typedef enum
{
    SCENE_NONE,
    SCENE_CLEAR,
    SCENE_PULSE,
    SCENE_AURA,
    SCENE_BEAM,
    SCENE_TEXT,
    SCENE_LIGHT,
    SCENE_ORBI,
    SCENE_DIAL,
} scene_t;

typedef struct
{
    scene_t scene;
    uint32_t color;
    int angle;
    bool phase_in;
    int hold_ms;
    int value;
    int level;
    char text[64];
    char name[16];
    char mood[8];
    char action[8];
    char seed[17];     /* Orbi child: 16 hex digits, "" = the father */
} job_t;

static lv_obj_t *s_root;          /* the current scene */
static scene_t s_current;
static lv_timer_t *s_hold;
static lv_obj_t *s_mic;           /* "microphone on" indicator, on lv_layer_sys() */
static bool s_light;

/* Beam geometry, valid while the beam animates. */
static lv_obj_t *s_comet;
static lv_obj_t *s_trail;
static lv_point_precise_t s_trail_points[2];
static lv_point_t s_beam_from, s_beam_to;

/* Dial. */
static lv_obj_t *s_arc;
static lv_obj_t *s_dial_label;
static char s_dial_text[64];
static int s_dial_value, s_dial_last;

/* Orbi. */
static lv_obj_t *s_pet;            /* container moved by enter/exit/bob */
static lv_obj_t *s_caption;
static orbi_view_t *s_orbi;        /* freed with the scene's objects */
static char s_orbi_name[16];

static int screen_w(void)
{
    return lv_display_get_horizontal_resolution(NULL);
}

static int screen_h(void)
{
    return lv_display_get_vertical_resolution(NULL);
}

/* ----------------------------------------------------------------- basics */

static void clear_scene(void)
{
    if (s_hold != NULL)
    {
        lv_timer_delete(s_hold);
        s_hold = NULL;
    }
    if (s_root != NULL)
    {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    if (s_current == SCENE_DIAL)
    {
        lv_port_set_knob_hook(NULL);
    }
    if (s_light)
    {
        power_keep_screen_on(false);
        s_light = false;
    }
    s_current = SCENE_NONE;
    s_comet = s_trail = NULL;
    s_arc = s_dial_label = NULL;
    s_pet = s_caption = NULL;
    s_orbi = NULL;
}

static void hold_done(lv_timer_t *timer)
{
    (void)timer;
    s_hold = NULL;   /* one-shot: LVGL deletes it */
    clear_scene();
}

static void hold_for(int ms)
{
    if (ms <= 0)
    {
        return;
    }
    s_hold = lv_timer_create(hold_done, (uint32_t)ms, NULL);
    lv_timer_set_repeat_count(s_hold, 1);
}

/* Full-screen transparent root; `modal` roots catch every touch. */
static lv_obj_t *make_root(scene_t scene, bool modal)
{
    clear_scene();
    s_current = scene;
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, screen_w(), screen_h());
    lv_obj_set_scrollable(s_root, false);
    if (!modal)
    {
        lv_obj_set_clickable(s_root, false);
    }
    return s_root;
}

static lv_obj_t *dot(lv_obj_t *parent, int size, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    return o;
}

static void glow(lv_obj_t *o, uint32_t color, int width)
{
    lv_obj_set_style_shadow_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(o, width, 0);
    lv_obj_set_style_shadow_spread(o, width / 6, 0);
    lv_obj_set_style_shadow_opa(o, LV_OPA_80, 0);
}

static void anim(void *var, lv_anim_exec_xcb_t exec, int from, int to, uint32_t ms, uint32_t delay,
                 lv_anim_path_cb_t path, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, path != NULL ? path : lv_anim_path_ease_out);
    if (done != NULL)
    {
        lv_anim_set_completed_cb(&a, done);
    }
    lv_anim_start(&a);
}

static void set_opa(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void set_translate_y(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y(obj, v, 0);
}

/* ------------------------------------------------------------------ pulse */

static void pulse_step(void *obj, int32_t v)
{
    int max = screen_w() > screen_h() ? screen_w() : screen_h();
    int size = 20 + (max - 20) * v / 1000;
    lv_obj_set_size(obj, size, size);
    lv_obj_center(obj);
    lv_obj_set_style_border_opa(obj, (lv_opa_t)(255 - v * 255 / 1000), 0);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)(90 - v * 90 / 1000), 0);
}

static void ring(lv_obj_t *parent, uint32_t color, uint32_t delay)
{
    lv_obj_t *o = dot(parent, 20, color);
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(o, 6, 0);
    glow(o, color, 30);
    lv_obj_center(o);
    anim(o, pulse_step, 0, 1000, 700, delay, lv_anim_path_ease_out, NULL);
}

static void scene_pulse(const job_t *j)
{
    lv_obj_t *root = make_root(SCENE_PULSE, false);
    ring(root, j->color, 0);
    ring(root, j->color, 160);
    hold_for(j->hold_ms > 0 ? j->hold_ms : 900);
}

/* ------------------------------------------------------------------- aura */

static void scene_aura(const job_t *j)
{
    lv_obj_t *root = make_root(SCENE_AURA, false);
    lv_obj_t *o = lv_obj_create(root);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, screen_w(), screen_h());
    lv_obj_set_style_radius(o, board_info()->round ? LV_RADIUS_CIRCLE : 48, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(j->color), 0);
    lv_obj_set_style_border_width(o, 16, 0);
    lv_obj_set_style_shadow_color(o, lv_color_hex(j->color), 0);
    lv_obj_set_style_shadow_width(o, 60, 0);
    lv_obj_set_clickable(o, false);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, set_opa);
    lv_anim_set_values(&a, 90, 255);
    lv_anim_set_duration(&a, 1100);
    lv_anim_set_reverse_duration(&a, 1100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
    hold_for(j->hold_ms > 0 ? j->hold_ms : 6000);
}

/* ------------------------------------------------------------------- beam */

static void beam_step(void *obj, int32_t v)
{
    int x = s_beam_from.x + (s_beam_to.x - s_beam_from.x) * v / 1000;
    int y = s_beam_from.y + (s_beam_to.y - s_beam_from.y) * v / 1000;
    lv_obj_set_pos(obj, x - COMET / 2, y - COMET / 2);
    if (s_trail != NULL)
    {
        /* The tail follows a little behind the head. */
        int tail = v > 350 ? v - 350 : 0;
        s_trail_points[0].x = s_beam_from.x + (s_beam_to.x - s_beam_from.x) * tail / 1000;
        s_trail_points[0].y = s_beam_from.y + (s_beam_to.y - s_beam_from.y) * tail / 1000;
        s_trail_points[1].x = x;
        s_trail_points[1].y = y;
        lv_line_set_points(s_trail, s_trail_points, 2);
    }
}

static void beam_arrived(lv_anim_t *a)
{
    (void)a;
    if (s_current == SCENE_BEAM && s_root != NULL)
    {
        uint32_t color = lv_color_to_u32(lv_obj_get_style_bg_color(s_comet, 0)) & 0xFFFFFF;
        lv_obj_set_hidden(s_comet, true);
        lv_obj_set_hidden(s_trail, true);
        ring(s_root, color, 0);
    }
}

static void scene_beam(const job_t *j)
{
    lv_obj_t *root = make_root(SCENE_BEAM, false);
    int w = screen_w(), h = screen_h();
    float a = (float)j->angle * DEG_TO_RAD;
    int reach = (w > h ? w : h) * 6 / 10;
    lv_point_t centre = {w / 2, h / 2};
    lv_point_t edge = {w / 2 + (int)(cosf(a) * reach), h / 2 + (int)(sinf(a) * reach)};
    s_beam_from = j->phase_in ? edge : centre;
    s_beam_to = j->phase_in ? centre : edge;
    s_trail = lv_line_create(root);
    lv_obj_set_style_line_color(s_trail, lv_color_hex(j->color), 0);
    lv_obj_set_style_line_width(s_trail, 10, 0);
    lv_obj_set_style_line_rounded(s_trail, true, 0);
    lv_obj_set_style_line_opa(s_trail, LV_OPA_70, 0);
    s_comet = dot(root, COMET, j->color);
    lv_obj_set_style_border_color(s_comet, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_comet, 4, 0);
    glow(s_comet, j->color, 40);
    if (j->text[0] != '\0')
    {
        lv_obj_t *label = lv_label_create(root);
        lv_label_set_text(label, j->text);
        lv_obj_set_style_text_color(label, lv_color_hex(j->color), 0);
        lv_obj_set_style_text_font(label, ui_font_body, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -h / 6);
    }
    beam_step(s_comet, 0);
    anim(s_comet, beam_step, 0, 1000, 480, 0, j->phase_in ? lv_anim_path_ease_out : lv_anim_path_ease_in,
         j->phase_in ? beam_arrived : NULL);
    if (!j->phase_in)
    {
        /* Leaving: the screen flashes where the comet started. */
        ring(root, j->color, 0);
    }
    hold_for(j->hold_ms > 0 ? j->hold_ms : 1300);
}

/* ------------------------------------------------------------ text, light */

static void on_dismiss(lv_event_t *e)
{
    (void)e;
    link_event_text("tap", s_current == SCENE_LIGHT ? "light" : "text");
    clear_scene();
}

static void scene_text(const job_t *j)
{
    lv_obj_t *root = make_root(SCENE_TEXT, true);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_70, 0);
    lv_obj_add_event_cb(root, on_dismiss, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *frame = lv_obj_create(root);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, screen_w() * 3 / 4, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(frame, 22, 0);
    lv_obj_set_style_radius(frame, 24, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(j->color), 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0x050A16), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_90, 0);
    glow(frame, j->color, 36);
    lv_obj_set_clickable(frame, false);
    lv_obj_center(frame);
    lv_obj_t *label = lv_label_create(frame);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(j->color), 0);
    lv_obj_set_style_text_font(label, ui_font_headline, 0);
    lv_label_set_text(label, j->text);
    lv_obj_set_style_opa(frame, LV_OPA_TRANSP, 0);
    anim(frame, set_opa, 0, 255, 350, 0, NULL, NULL);
    hold_for(j->hold_ms > 0 ? j->hold_ms : 6000);
}

static void scene_light(const job_t *j)
{
    lv_obj_t *root = make_root(SCENE_LIGHT, true);
    lv_obj_set_style_bg_color(root, lv_color_hex(j->color), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(root, on_dismiss, LV_EVENT_SHORT_CLICKED, NULL);
    power_keep_screen_on(true);
    s_light = true;
    hold_for(j->hold_ms > 0 ? j->hold_ms : 600000);
}

/* ------------------------------------------------------------------- dial */

static void dial_show(void)
{
    if (s_arc == NULL)
    {
        return;
    }
    lv_arc_set_value(s_arc, s_dial_value);
    s_dial_last = s_dial_value;
    lv_label_set_text_fmt(s_dial_label, "%s\n%d%%", s_dial_text, s_dial_value);
}

static bool dial_knob(int delta)
{
    if (s_current != SCENE_DIAL)
    {
        return false;
    }
    s_dial_value += delta * 2;
    s_dial_value = s_dial_value < 0 ? 0 : (s_dial_value > 100 ? 100 : s_dial_value);
    dial_show();
    link_event("dial", delta);
    return true;
}

static void on_arc(lv_event_t *e)
{
    (void)e;
    int value = lv_arc_get_value(s_arc);
    int steps = (value - s_dial_last) / 2;
    if (steps != 0)
    {
        s_dial_last += steps * 2;
        s_dial_value = value;
        lv_label_set_text_fmt(s_dial_label, "%s\n%d%%", s_dial_text, s_dial_value);
        link_event("dial", steps);
    }
}

static void on_dial_done(lv_event_t *e)
{
    (void)e;
    link_event("dial_done", s_dial_value);
    clear_scene();
}

static void scene_dial(const job_t *j)
{
    if (j->value >= 0)
    {
        s_dial_value = j->value > 100 ? 100 : j->value;
    }
    snprintf(s_dial_text, sizeof(s_dial_text), "%s", j->text[0] != '\0' ? j->text : "Orbit");
    if (s_current == SCENE_DIAL)
    {
        dial_show();   /* an update from Orbit: no flicker */
        return;
    }
    lv_obj_t *root = make_root(SCENE_DIAL, true);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x02040A), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_90, 0);
    int size = (screen_w() < screen_h() ? screen_w() : screen_h()) * 82 / 100;
    s_arc = lv_arc_create(root);
    lv_obj_set_size(s_arc, size, size);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_bg_angles(s_arc, 135, 45);
    lv_obj_set_style_arc_width(s_arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x1B2A46), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(j->color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_arc, lv_color_hex(j->color), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc, 6, LV_PART_KNOB);
    lv_obj_center(s_arc);
    lv_obj_add_event_cb(s_arc, on_arc, LV_EVENT_VALUE_CHANGED, NULL);
    s_dial_label = lv_label_create(root);
    lv_obj_set_width(s_dial_label, size * 6 / 10);
    lv_label_set_long_mode(s_dial_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_dial_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_dial_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_dial_label, ui_font_body, 0);
    lv_obj_align(s_dial_label, LV_ALIGN_CENTER, 0, -10);
    lv_obj_t *done = ui_button(root, "Fine", 0x1C2B48, on_dial_done, NULL);
    lv_obj_set_width(done, size / 2);
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -screen_h() / 9);
    lv_port_set_knob_hook(dial_knob);
    dial_show();
}

/* ------------------------------------------------------------------- Orbi */

/* Orbi the father, or a child when the companion sends its seed. Drawn by
 * components/orbi exactly like the companion draws it. */
#define ORBI_FATHER_SEED 0x0d0000bb00000245ULL

static orbi_mood_t orbi_mood_of(const job_t *j)
{
    if (strcmp(j->action, "love") == 0)
    {
        return ORBI_LOVE;
    }
    if (strcmp(j->mood, "sleep") == 0)
    {
        return ORBI_SLEEP;
    }
    if (strcmp(j->mood, "tired") == 0)
    {
        return ORBI_SLEEPY;
    }
    if (strcmp(j->mood, "sad") == 0)
    {
        return ORBI_SAD;
    }
    return strcmp(j->mood, "happy") == 0 ? ORBI_HAPPY : ORBI_CALM;
}

static void orbi_face(const job_t *j)
{
    orbi_view_mood(s_orbi, orbi_mood_of(j));
    char caption[48];
    snprintf(caption, sizeof(caption), "%s  LV %d", j->name[0] != '\0' ? j->name : s_orbi_name, j->level > 0 ? j->level : 1);
    lv_label_set_text(s_caption, caption);
}

static void on_orbi(lv_event_t *e)
{
    bool long_press = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED;
    link_event_text("orbi", long_press ? "play" : "pet");
    if (s_orbi != NULL)
    {
        orbi_view_poke(s_orbi);
    }
}

static void exit_done(lv_anim_t *a)
{
    (void)a;
    if (s_current == SCENE_ORBI)
    {
        clear_scene();
    }
}

static void set_translate_x(void *obj, int32_t v)
{
    lv_obj_set_style_translate_x(obj, v, 0);
}

static void scene_orbi(const job_t *j)
{
    bool enter = strcmp(j->action, "enter") == 0, leave = strcmp(j->action, "exit") == 0;
    if (s_current != SCENE_ORBI || enter)
    {
        lv_obj_t *root = make_root(SCENE_ORBI, false);
        int w = screen_w(), h = screen_h();
        int s = (w < h ? w : h) * 30 / 100;
        uint64_t seed = ORBI_FATHER_SEED;
        bool child = j->seed[0] != '\0' && orbi_parse(j->seed, &seed);
        orbi_genome_t g;
        orbi_genome(seed, &g);
        snprintf(s_orbi_name, sizeof(s_orbi_name), "%s", child ? g.name : "Orbi");

        s_pet = lv_obj_create(root);
        lv_obj_remove_style_all(s_pet);
        lv_obj_set_size(s_pet, s * 3 / 2, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(s_pet, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_pet, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(s_pet, false);
        lv_obj_add_event_cb(s_pet, on_orbi, LV_EVENT_SHORT_CLICKED, NULL);
        lv_obj_add_event_cb(s_pet, on_orbi, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_align(s_pet, LV_ALIGN_CENTER, 0, h / 10);
        s_orbi = orbi_view_create(s_pet, &g, s);
        s_caption = lv_label_create(s_pet);
        lv_obj_set_style_text_color(s_caption, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_caption, ui_font_small, 0);
        if (enter)
        {
            float rad = (float)j->angle * DEG_TO_RAD;
            int reach = (w > h ? w : h) * 7 / 10;
            anim(s_pet, set_translate_x, (int)(cosf(rad) * reach), 0, 700, 0, lv_anim_path_ease_out, NULL);
            anim(s_pet, set_translate_y, (int)(sinf(rad) * reach), 0, 700, 0, lv_anim_path_ease_out, NULL);
        }
    }
    if (s_orbi == NULL)
    {
        return;
    }
    orbi_face(j);
    if (leave)
    {
        float rad = (float)j->angle * DEG_TO_RAD;
        int reach = (screen_w() > screen_h() ? screen_w() : screen_h()) * 7 / 10;
        lv_anim_delete(s_pet, NULL);
        anim(s_pet, set_translate_x, 0, (int)(cosf(rad) * reach), 600, 0, lv_anim_path_ease_in, NULL);
        anim(s_pet, set_translate_y, 0, (int)(sinf(rad) * reach), 600, 0, lv_anim_path_ease_in, exit_done);
    }
    else if (strcmp(j->action, "eat") == 0 || strcmp(j->action, "play") == 0 || strcmp(j->action, "love") == 0)
    {
        orbi_view_poke(s_orbi);
    }
    if (j->hold_ms > 0)
    {
        hold_for(j->hold_ms);
    }
}

/* --------------------------------------------------------------- dispatch */

static void apply(void *arg)
{
    job_t *j = arg;
    if (j->scene != SCENE_CLEAR && j->scene != SCENE_NONE)
    {
        power_wake(POWER_WAKE_OTHER);
    }
    switch (j->scene)
    {
    case SCENE_PULSE:
        scene_pulse(j);
        break;
    case SCENE_AURA:
        scene_aura(j);
        break;
    case SCENE_BEAM:
        scene_beam(j);
        break;
    case SCENE_TEXT:
        scene_text(j);
        break;
    case SCENE_LIGHT:
        scene_light(j);
        break;
    case SCENE_ORBI:
        scene_orbi(j);
        break;
    case SCENE_DIAL:
        scene_dial(j);
        break;
    default:
        clear_scene();
        break;
    }
    free(j);
}

static void copy_text(cJSON *json, const char *key, char *out, size_t size)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, key));
    snprintf(out, size, "%s", v != NULL ? v : "");
}

static int number(cJSON *json, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

bool link_scene_post(cJSON *json)
{
    static const struct
    {
        const char *name;
        scene_t scene;
    } names[] = {
        {"clear", SCENE_CLEAR}, {"pulse", SCENE_PULSE}, {"aura", SCENE_AURA}, {"beam", SCENE_BEAM},
        {"text", SCENE_TEXT},   {"light", SCENE_LIGHT}, {"orbi", SCENE_ORBI}, {"dial", SCENE_DIAL},
    };
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "scene"));
    scene_t scene = SCENE_NONE;
    for (size_t i = 0; name != NULL && i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (strcmp(name, names[i].name) == 0)
        {
            scene = names[i].scene;
        }
    }
    job_t *j = scene != SCENE_NONE ? heap_caps_calloc(1, sizeof(job_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (j == NULL)
    {
        cJSON_Delete(json);
        return false;
    }
    j->scene = scene;
    j->color = (uint32_t)number(json, "color", 0x3CF0FF) & 0xFFFFFF;
    j->angle = number(json, "angle", 0);
    j->hold_ms = number(json, "hold_ms", -1);
    j->value = number(json, "value", -1);
    j->level = number(json, "level", 1);
    const char *phase = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "phase"));
    j->phase_in = phase != NULL && strcmp(phase, "in") == 0;
    copy_text(json, "text", j->text, sizeof(j->text));
    copy_text(json, "name", j->name, sizeof(j->name));
    copy_text(json, "mood", j->mood, sizeof(j->mood));
    copy_text(json, "action", j->action, sizeof(j->action));
    copy_text(json, "seed", j->seed, sizeof(j->seed));
    cJSON_Delete(json);
    if (!ui_async(apply, j))
    {
        free(j);
        return false;
    }
    return true;
}

/* ------------------------------------------------------- mic indicator */

static void mic_apply(void *arg)
{
    bool on = arg != NULL;
    if (!on)
    {
        if (s_mic != NULL)
        {
            lv_obj_delete(s_mic);
            s_mic = NULL;
        }
        return;
    }
    if (s_mic != NULL)
    {
        return;
    }
    s_mic = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_mic);
    lv_obj_set_size(s_mic, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(s_mic, 14, 0);
    lv_obj_set_style_pad_ver(s_mic, 6, 0);
    lv_obj_set_style_radius(s_mic, 20, 0);
    lv_obj_set_style_bg_color(s_mic, lv_color_hex(0xFF2D55), 0);
    lv_obj_set_style_bg_opa(s_mic, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_mic, false);
    lv_obj_t *label = lv_label_create(s_mic);
    lv_label_set_text(label, LV_SYMBOL_AUDIO " Orbit ascolta");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_font_small, 0);
    lv_obj_align(s_mic, LV_ALIGN_TOP_MID, 0, screen_h() / 10);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_mic);
    lv_anim_set_exec_cb(&a, set_opa);
    lv_anim_set_values(&a, 140, 255);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_reverse_duration(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

void link_scene_mic(bool on)
{
    ui_async(mic_apply, on ? (void *)1 : NULL);
}
