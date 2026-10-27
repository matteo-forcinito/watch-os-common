/* Standalone DJ and Studio; the optional paired hub starts its radio on request. */
#include "core/app.h"
#include "core/clock.h"
#include "core/devcon.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "engine.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "hardware/imu.h"
#include "install.h"
#include "layout.h"
#include "ui/ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_title[2], *s_time[2], *s_play[2], *s_cross, *s_volume, *s_meter, *s_info;
static lv_obj_t *s_cross_text, *s_volume_text;
static lv_obj_t *s_wave_obj[2];
static uint16_t s_wave[2][64];
static bool s_wave_active[2];
static solco_status_t s_previous;
static bool s_first = true;
static int s_width, s_height;
static bool s_landscape;
static solco_layout_t s_layout;
static lv_obj_t *s_mixer, *s_detail, *s_detail_title, *s_detail_time, *s_detail_play, *s_disc;
static lv_obj_t *s_mute,*s_quick_play;
static int s_deck;
static int s_restore_volume = 10;
static float s_disc_angle, s_touch_angle;
static lv_image_dsc_t s_grooves;
static bool s_disc_drag, s_touch_valid;
static lv_obj_t *s_deck_menu, *s_loop_text, *s_studio_tools, *s_studio_menu;
static lv_obj_t *s_loop_toggle, *s_rate_panel, *s_rate_text, *s_keylock_button;
static uint32_t s_loop_error_until;
static lv_obj_t *s_hint, *s_brand, *s_library, *s_library_list, *s_library_name;
static int s_library_deck;
static lv_obj_t *s_master, *s_detail_volume, *s_wheel_mode, *s_record_label;
static bool s_volume_open, s_wheel_vinyl = true;
static lv_obj_t *s_studio_page, *s_studio_name, *s_studio_info, *s_studio_play, *s_studio_rec;
static lv_obj_t *s_pad[8], *s_steps[16], *s_patch, *s_link_info, *s_motion_label, *s_studio_volume,
    *s_kit;
static int s_studio_view, s_instrument;
static void studio_open(int view);
static void sync_master(void);
static void deck_menu(lv_event_t *event);
static void detail_volume(lv_event_t *event);
static void update_wheel_mode(void);
static void open_library(int deck);

static int x(int value) { return value * s_width / 360; }
static int y(int value) { return value * s_height / 360; }

static lv_obj_t *label(lv_obj_t *parent, const char *text, int left, int top, int width,
                       uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x(left), y(top));
    lv_obj_set_width(obj, x(width));
    lv_obj_set_style_text_font(obj, s_landscape ? &lv_font_montserrat_16 : &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    return obj;
}

static void action(lv_event_t *event)
{
    intptr_t code = (intptr_t)lv_event_get_user_data(event);
    int deck = code / 3;
    if (code % 3 == 0)
    {
        solco_toggle(deck);
    }
    else if (code % 3 == 1)
    {
        solco_cue(deck);
    }
    else
    {
        open_library(deck);
    }
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int left, int top, int width,
                        uint32_t color, int code)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_set_pos(obj, x(left), y(top));
    lv_obj_set_size(obj, x(width), y(42));
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_ext_click_area(obj, y(4));
    lv_obj_add_event_cb(obj, action, LV_EVENT_CLICKED, (void *)(intptr_t)code);
    lv_obj_t *text_obj = lv_label_create(obj);
    lv_label_set_text(text_obj, text);
    lv_obj_set_style_text_font(text_obj, s_landscape ? &lv_font_montserrat_16 : &lv_font_montserrat_14, 0);
    lv_obj_center(text_obj);
    return text_obj;
}

static void cross_event(lv_event_t *event)
{
    solco_cross(lv_slider_get_value(lv_event_get_target_obj(event)));
}

static void volume_event(lv_event_t *event)
{
    solco_volume(lv_slider_get_value(lv_event_get_target_obj(event)));
}

static void knob_event(lv_event_t *event)
{
    lv_port_knob_t *knob = lv_event_get_param(event);
    solco_status_t status;
    solco_status(&status);
    if (!lv_obj_is_hidden(s_detail) && lv_obj_is_hidden(s_library) && !s_volume_open &&
        s_wheel_vinyl)
    {
        solco_jog(s_deck, knob->delta);
    }
    else
    {
        solco_volume(status.volume + knob->delta);
    }
    knob->handled = true;
}

static lv_obj_t *slider(lv_obj_t *home, int top, int max, int value, lv_event_cb_t callback)
{
    lv_obj_t *obj = lv_slider_create(home);
    lv_obj_set_pos(obj, x(72), y(top));
    lv_obj_set_size(obj, x(216), y(10));
    lv_slider_set_range(obj, 0, max);
    lv_slider_set_value(obj, value, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(obj, y(16));
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x273341), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x69E4CE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xE5F8F4), LV_PART_KNOB);
    lv_obj_add_event_cb(obj, callback, LV_EVENT_VALUE_CHANGED, NULL);
    return obj;
}

static void draw_wave(lv_event_t *event)
{
    int deck = (int)(intptr_t)lv_event_get_user_data(event);
    lv_obj_t *obj = lv_event_get_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int width = lv_area_get_width(&area);
    int half = lv_area_get_height(&area) / 2;
    int middle = area.y1 + half;
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = 1;
    line.color = lv_color_hex(0x304252);
    line.p1 = (lv_point_precise_t){area.x1, middle};
    line.p2 = (lv_point_precise_t){area.x2, middle};
    lv_draw_line(layer, &line);
    line.width = 2;
    line.color = lv_color_hex(deck == 0 ? 0x69E4CE : 0xFFBA78);
    line.opa = s_wave_active[deck] ? LV_OPA_COVER : LV_OPA_60;
    uint16_t maximum = 1;
    for (int i = 0; i < 64; ++i)
    {
        if (s_wave[deck][i] > maximum)
        {
            maximum = s_wave[deck][i];
        }
    }
    for (int i = 0; i < 64; ++i)
    {
        int amplitude = s_wave[deck][i] * (half - 1) / maximum;
        if (s_wave[deck][i] && amplitude == 0)
        {
            amplitude = 1;
        }
        int left = area.x1 + i * (width - 1) / 63;
        line.p1 = (lv_point_precise_t){left, middle - amplitude};
        line.p2 = (lv_point_precise_t){left, middle + amplitude};
        lv_draw_line(layer, &line);
    }
}

static void volume_step(lv_event_t *event)
{
    solco_status_t status;
    solco_status(&status);
    int step = (int)(intptr_t)lv_event_get_user_data(event);
    if (step == 0)
    {
        if (status.volume > 0)
        {
            s_restore_volume = status.volume;
            solco_volume(0);
        }
        else
        {
            solco_volume(s_restore_volume);
        }
    }
    else
    {
        solco_volume(status.volume + step);
    }
}

static void show_deck(lv_event_t *event)
{
    solco_scratch(s_deck, 2, 0);
    s_disc_drag = false;
    s_deck = (int)(intptr_t)lv_event_get_user_data(event);
    s_volume_open = false;
    lv_obj_set_hidden(s_deck_menu, true);
    lv_obj_set_hidden(s_brand, true);
    lv_obj_set_hidden(s_mixer, true);
    lv_obj_set_hidden(s_detail, false);
    update_wheel_mode();
    sync_master();
    s_first = true;
}

static void show_mixer(lv_event_t *event)
{
    (void)event;
    solco_scratch(s_deck, 2, 0);
    s_disc_drag = false;
    lv_obj_set_hidden(s_detail, true);
    lv_obj_set_hidden(s_deck_menu, true);
    lv_obj_set_hidden(s_brand, false);
    lv_obj_set_hidden(s_mixer, false);
    s_volume_open = false;
    sync_master();
}

static void detail_action(lv_event_t *event)
{
    int code = (int)(intptr_t)lv_event_get_user_data(event);
    if (code == 0)
    {
        solco_toggle(s_deck);
    }
    else if (code == 1)
    {
        solco_cue(s_deck);
    }
    else
    {
        solco_scratch(s_deck, 2, 0);
        open_library(s_deck);
    }
}

static void deck_menu(lv_event_t *event)
{
    (void)event;
    solco_scratch(s_deck, 2, 0);
    s_disc_drag = s_touch_valid = false;
    lv_obj_set_hidden(s_rate_panel, true);
    lv_obj_set_hidden(s_deck_menu, !lv_obj_is_hidden(s_deck_menu));
    if (lv_obj_is_hidden(s_deck_menu))
    {
        s_volume_open = false;
        sync_master();
    }
}
static void loop_action(lv_event_t *event)
{
    if (!solco_loop(s_deck, (int)(intptr_t)lv_event_get_user_data(event)))
    {
        s_loop_error_until = lv_tick_get() + 1800;
    }
}

static void rate_action(lv_event_t *event)
{
    int rate = (int)(intptr_t)lv_event_get_user_data(event);
    if (!rate)
    {
        s_volume_open = false;
        sync_master();
        lv_obj_set_hidden(s_rate_panel, !lv_obj_is_hidden(s_rate_panel));
    }
    else
        solco_rate(s_deck, rate);
}

static void keylock_action(lv_event_t *event)
{
    (void)event;
    solco_status_t status;
    solco_status(&status);
    solco_keylock(s_deck, !status.keylock[s_deck]);
}

static void platter_touch(lv_event_t *event)
{
    if (s_volume_open || !lv_obj_is_hidden(s_deck_menu))
    {
        return;
    }
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        if (s_disc_drag)
        {
            solco_scratch(s_deck, 2, 0);
        }
        s_disc_drag = false;
        s_touch_valid = false;
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING)
    {
        return;
    }
    lv_indev_t *input = lv_indev_active();
    if (!input)
    {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(input, &point);
    lv_area_t area;
    lv_obj_get_coords(s_disc, &area);
    float dx = point.x - (area.x1 + area.x2) * 0.5f;
    float dy = point.y - (area.y1 + area.y2) * 0.5f;
    float deadzone = lv_area_get_width(&area) * .17f;
    if (dx * dx + dy * dy < deadzone * deadzone)
    {
        s_touch_valid = false;
        return; /* Never unwrap an angle across the menu/spindle. */
    }
    float angle = atan2f(dy, dx);
    if (code == LV_EVENT_PRESSED || !s_touch_valid)
    {
        s_touch_valid = true;
        s_disc_drag = true;
        s_touch_angle = angle;
        solco_scratch(s_deck, 0, 0);
    }
    else if (s_disc_drag)
    {
        float delta = angle - s_touch_angle;
        if (delta > 3.14159265f)
        {
            delta -= 6.2831853f;
        }
        if (delta < -3.14159265f)
        {
            delta += 6.2831853f;
        }
        s_touch_angle = angle;
        /* 33 1/3 RPM platter: one revolution = 1.8 seconds of source audio. */
        solco_scratch(s_deck, 1, (int32_t)(delta * (44100 * 1.8f / 6.2831853f)));
    }
}

static void invalidate_marker(float angle)
{
    lv_area_t disc;
    lv_obj_get_coords(s_disc, &disc);
    float cx = (disc.x1 + disc.x2) * .5f, cy = (disc.y1 + disc.y2) * .5f;
    float radius = lv_area_get_width(&disc) * .4f;
    float ax = cx + cosf(angle) * radius * .65f, ay = cy + sinf(angle) * radius * .65f;
    float bx = cx + cosf(angle) * radius, by = cy + sinf(angle) * radius;
    lv_area_t dirty = {(int32_t)fminf(ax, bx) - 6, (int32_t)fminf(ay, by) - 6,
                       (int32_t)fmaxf(ax, bx) + 6, (int32_t)fmaxf(ay, by) + 6};
    lv_obj_invalidate_area(s_disc, &dirty);
}
static uint16_t rgb565(uint32_t color)
{
    return (uint16_t)(((color >> 8) & 0xF800) | ((color >> 5) & 0x07E0) | ((color >> 3) & 0x1F));
}
static void create_grooves(int diameter)
{
    uint16_t *pixels = heap_caps_malloc(diameter * diameter * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!pixels)
        return; /* Audio remains usable without decorative grooves. */
    float center = (diameter - 1) * .5f;
    for (int row = 0; row < diameter; ++row)
    {
        for (int col = 0; col < diameter; ++col)
        {
            float dx = col - center, dy = row - center;
            float radius = sqrtf(dx * dx + dy * dy);
            uint32_t color = radius > center ? 0x080E16 : 0x0C151F;
            if (fabsf(radius - (center - 1)) < 1)
                color = 0x35485B;
            for (int ring = 0; ring < 14 && radius < center - 2; ++ring)
            {
                float r = diameter * (95 - ring * 4) / 200.f;
                if (fabsf(radius - r) < (ring % 4 ? .55f : 1.f))
                {
                    color = ring % 4 ? 0x182A3A : 0x263B4D;
                    break;
                }
            }
            pixels[row * diameter + col] = rgb565(color);
        }
    }
    s_grooves.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_grooves.header.cf = LV_COLOR_FORMAT_RGB565;
    s_grooves.header.w = s_grooves.header.h = diameter;
    s_grooves.header.stride = diameter * 2;
    s_grooves.data_size = diameter * diameter * 2;
    s_grooves.data = (const uint8_t *)pixels;
    lv_obj_t *image = lv_image_create(s_disc);
    lv_image_set_src(image, &s_grooves);
    lv_obj_set_clickable(image, false);
    lv_obj_set_scrollable(image, false);
}

static void draw_disc(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    int cx = (area.x1 + area.x2) / 2;
    int cy = (area.y1 + area.y2) / 2;
    int radius = lv_area_get_width(&area) * 40 / 100;
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = 4;
    line.color = lv_color_hex(s_deck == 0 ? 0x69E4CE : 0xFFBA78);
    line.p1 = (lv_point_precise_t){cx + cosf(s_disc_angle) * radius * 0.65f,
                                   cy + sinf(s_disc_angle) * radius * 0.65f};
    line.p2 =
        (lv_point_precise_t){cx + cosf(s_disc_angle) * radius, cy + sinf(s_disc_angle) * radius};
    lv_draw_line(lv_event_get_layer(event), &line);
}

static lv_obj_t *container(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, s_width, y(274));
    lv_obj_set_scrollable(obj, false);
    return obj;
}

static lv_obj_t *control(lv_obj_t *parent, const char *text, int left, int top, int width,
                         lv_event_cb_t callback, int code)
{
    lv_obj_t *obj = button(parent, text, left, top, width, 0x243747, 0);
    lv_obj_t *btn = lv_obj_get_parent(obj);
    lv_obj_remove_event_cb(btn, action);
    lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, (void *)(intptr_t)code);
    return obj;
}

static void studio_action(lv_event_t *event)
{
    int code = (int)(intptr_t)lv_event_get_user_data(event);
    studio_status_t state;
    solco_studio_status(&state);
    if (code >= 100 && code < 108)
    {
        solco_studio_command(STUDIO_HIT,
                             (s_studio_view == 2 ? 8 + s_instrument * 8 : 0) + code - 100, 110);
    }
    else if (code == 10)
    {
        solco_studio_command(STUDIO_PLAY, 0, 0);
    }
    else if (code == 11)
    {
        solco_studio_command(STUDIO_RECORD, 0, 0);
    }
    else if (code == 12)
    {
        solco_studio_command(STUDIO_CLEAR, 0, 0);
    }
    else if (code == 13)
    {
        s_instrument = (s_instrument + 1) % 3;
        studio_open(2);
    }
    else if (code == 14)
    {
        solco_studio_command(STUDIO_TEMPO, state.bpm - 5, 0);
    }
    else if (code == 15)
    {
        solco_studio_command(STUDIO_TEMPO, state.bpm + 5, 0);
    }
    else if (code == 18)
    {
        lv_obj_set_hidden(s_studio_tools, !lv_obj_is_hidden(s_studio_tools));
    }
    else if (code == 19)
    {
        solco_motion_calibrate();
    }
    else if (code == 17)
    {
        solco_studio_command(STUDIO_KIT, state.kit ? 0 : 1, 0);
    }
    else if (code == 16)
    {
        solco_motion_mode((solco_motion_get() + 1) % 3);
    }
    else
    {
        studio_open(code);
    }
}
static void studio_open(int view)
{
    s_studio_view = view;
    if (s_studio_tools)
        lv_obj_set_hidden(s_studio_tools, true);
    solco_studio_command(STUDIO_ENABLE, view == 1 || view == 2, 0);
    lv_obj_set_hidden(s_studio_page, view < 0);
    lv_obj_set_hidden(s_mixer, view >= 0);
    lv_obj_set_hidden(s_brand, view >= 0);
    lv_obj_set_hidden(s_detail, true);
    lv_obj_set_hidden(s_library, true);
    s_volume_open = false;
    sync_master();
    if (view < 0)
    {
        return;
    }
    const char *names[] = {"STUDIO", "DRUMPAD", "STRUMENTI", "MOVIMENTO"};
    lv_label_set_text(s_studio_name, names[view]);
    for (int i = 0; i < 8; i++)
    {
        lv_obj_set_hidden(lv_obj_get_parent(s_pad[i]), view != 1 && view != 2);
    }
    for (int i = 0; i < 16; i++)
    {
        lv_obj_set_hidden(s_steps[i], view != 1);
    }
    lv_obj_set_hidden(lv_obj_get_parent(s_studio_play), view != 1);
    lv_obj_set_hidden(lv_obj_get_parent(s_studio_rec), view != 1);
    lv_obj_set_hidden(lv_obj_get_parent(s_kit), view != 1);
    lv_obj_set_hidden(lv_obj_get_parent(s_patch), view != 2);
    lv_obj_set_hidden(lv_obj_get_parent(s_studio_menu), view != 1 && view != 2);
    static const char *patches[] = {"E-PIANO   >", "BASS   >", "BELL   >"};
    lv_label_set_text(s_patch, patches[s_instrument]);
    static const char *drums[] = {"KICK",     "SNARE", "CLAP",   "HI-HAT",
                                  "OPEN HAT", "TOM",   "SHAKER", "RIM"};
    static const char *notes[] = {"DO", "RE", "MI", "FA", "SOL", "LA", "SI", "DO +"};
    for (int i = 0; i < 8; i++)
    {
        lv_label_set_text(s_pad[i], view == 2 ? notes[i] : drums[i]);
    }
    /* Other panels are children tagged by their view through user data. */
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_studio_page); i++)
    {
        lv_obj_t *child = lv_obj_get_child(s_studio_page, i);
        intptr_t tag = (intptr_t)lv_obj_get_user_data(child);
        if (tag >= 1000)
        {
            lv_obj_set_hidden(child, view != tag - 1000);
        }
    }
}
static void studio_launch(lv_event_t *event)
{
    (void)event;
    studio_open(0);
}
static void studio_refresh(void)
{
    if (!s_studio_page || s_studio_view < 0)
    {
        return;
    }
    studio_status_t state;
    solco_studio_status(&state);
    if (s_studio_volume)
    {
        lv_label_set_text(s_studio_volume, s_volume_open ? "CHIUDI" : "VOL");
    }
    lv_label_set_text_fmt(s_studio_info, "%d BPM  /  %d VOCI", state.bpm, state.voices);
    lv_label_set_text(s_kit, state.kit ? "KIT B" : "KIT A");
    lv_label_set_text(s_studio_play, state.playing ? "STOP" : "PLAY");
    lv_label_set_text(s_studio_rec, state.recording ? "REC ON" : "REC");
    lv_obj_set_style_bg_color(lv_obj_get_parent(s_studio_rec),
                              lv_color_hex(state.recording ? 0xB33859 : 0x243747), 0);
    for (int i = 0; i < 16; i++)
    {
        lv_obj_set_style_bg_color(
            s_steps[i], lv_color_hex(state.playing && state.step == i ? 0x69E4CE : 0x283442), 0);
    }
    lv_label_set_text(s_link_info,
                      "BATTERIA: IMPARA COLPO\nFermo, poi un colpo in avanti.\nCentro: snare. "
                      "Inclina: kit.\nRileva la frenata del colpo.");
    const char *motion[] = {"MOVIMENTO: OFF", "MARACAS", "BATTERIA"};
    int learned, pad;
    solco_motion_state(&learned, &pad);
    if (imu_supported() && solco_motion_get() == 2)
    {
        static const char *drums[] = {"KICK",     "SNARE", "CLAP",   "HI-HAT",
                                      "OPEN HAT", "TOM",   "SHAKER", "RIM"};
        lv_label_set_text_fmt(s_motion_label, learned ? "BATTERIA: %s" : "FAI IL COLPO DI PROVA",
                              drums[pad & 7]);
    }
    else
        lv_label_set_text(s_motion_label,
                          imu_supported() ? motion[solco_motion_get()] : "NESSUNA IMU");
}
static void studio_tag(lv_obj_t *text, int view)
{
    lv_obj_set_user_data(lv_obj_get_parent(text), (void *)(intptr_t)(1000 + view));
}
static void create_studio(lv_obj_t *home)
{
    s_studio_view = -1;
    s_studio_page = container(home);
    lv_obj_set_height(s_studio_page, s_height);
    lv_obj_set_style_bg_color(s_studio_page, lv_color_hex(0x080E16), 0);
    lv_obj_set_style_bg_opa(s_studio_page, LV_OPA_COVER, 0);
    bool round = board_info()->round;
    s_studio_name = label(s_studio_page, "STUDIO", 112, round ? 32 : 13, 118, 0xE5F8F4);
    control(s_studio_page, "<", round ? 60 : 8, round ? 25 : 5, 42, studio_action, -1);
    s_studio_volume =
        control(s_studio_page, "VOL", round ? 246 : 296, round ? 25 : 5, 56, detail_volume, 0);
    studio_tag(control(s_studio_page, "MIX / VINYL", 65, 87, 230, studio_action, -1), 0);
    studio_tag(control(s_studio_page, "DRUMPAD / BEAT", 45, 137, 270, studio_action, 1), 0);
    studio_tag(control(s_studio_page, "STRUMENTI", 45, 187, 270, studio_action, 2), 0);
    studio_tag(control(s_studio_page, "MOVIMENTO", 65, 237, 230, studio_action, 3), 0);
    s_studio_play = control(s_studio_page, "PLAY", round ? 82 : 12, round ? 301 : 312,
                            round ? 60 : 102, studio_action, 10);
    s_studio_rec = control(s_studio_page, "REC", round ? 150 : 126, round ? 301 : 312,
                           round ? 60 : 102, studio_action, 11);
    s_studio_menu = control(s_studio_page, "...", round ? 218 : 240, round ? 301 : 312,
                            round ? 60 : 108, studio_action, 18);
    s_patch = control(s_studio_page, "E-PIANO >", round ? 82 : 12, round ? 301 : 312,
                      round ? 128 : 216, studio_action, 13);
    for (int i = 0; i < 8; i++)
    {
        solco_rect_t box=s_layout.pad[i];
        s_pad[i]=control(s_studio_page,"PAD",box.x,box.y,box.w,studio_action,100+i);
        lv_obj_t *pad=lv_obj_get_parent(s_pad[i]);
        lv_obj_set_height(pad,y(box.h));
        lv_obj_set_style_radius(pad, 12, 0);
        lv_obj_set_style_bg_color(pad, lv_color_hex((i%2) ? 0x603C32 : 0x1D4E4D), 0);
        lv_obj_remove_event_cb(pad, studio_action);
        lv_obj_add_event_cb(pad, studio_action, LV_EVENT_PRESSED, (void *)(intptr_t)(100 + i));
        lv_obj_set_style_bg_color(pad, lv_color_hex(0xA5F5DD), LV_STATE_PRESSED);
    }
    for (int i = 0; i < 16; i++)
    {
        s_steps[i] = lv_obj_create(s_studio_page);
        lv_obj_remove_style_all(s_steps[i]);
        lv_obj_set_pos(s_steps[i], x((round ? 52 : 12) + i * (round ? 16 : 21)),
                       y(round ? 69 : 48));
        lv_obj_set_size(s_steps[i], x(round ? 12 : 17), y(3));
        lv_obj_set_style_bg_opa(s_steps[i], LV_OPA_COVER, 0);
        lv_obj_set_clickable(s_steps[i], false);
    }
    s_studio_tools = container(s_studio_page);
    lv_obj_set_height(s_studio_tools, s_height);
    lv_obj_set_style_bg_color(s_studio_tools, lv_color_hex(0x101C28), 0);
    lv_obj_set_style_bg_opa(s_studio_tools, LV_OPA_COVER, 0);
    control(s_studio_tools, "< SUONA", 90, 30, 180, studio_action, 18);
    s_studio_info = label(s_studio_tools, "120 BPM", 90, 87, 210, 0xE5F8F4);
    control(s_studio_tools, "BPM -", 48, 118, 128, studio_action, 14);
    control(s_studio_tools, "BPM +", 184, 118, 128, studio_action, 15);
    s_kit = control(s_studio_tools, "KIT A", 48, 172, 128, studio_action, 17);
    control(s_studio_tools, "CLR BEAT", 184, 172, 128, studio_action, 12);
    label(s_studio_tools, "REC sovraincide  /  CLR cancella", 48, 238, 280, 0x8FA7BF);
    lv_obj_set_hidden(s_studio_tools, true);
    s_link_info = label(s_studio_page, "", 48, 116, 270, 0xAEC3D4);
    lv_obj_set_user_data(s_link_info, (void *)1003);
    lv_obj_set_height(s_link_info, y(round ? 122 : 87));
    lv_label_set_long_mode(s_link_info, LV_LABEL_LONG_WRAP);
    s_motion_label = control(s_studio_page, "MOVIMENTO: OFF", 60, 206, 240, studio_action, 16);
    studio_tag(s_motion_label, 3);
    control(s_studio_page, "IMPARA COLPO", 60, 258, 240, studio_action, 19);
    lv_obj_set_user_data(lv_obj_get_child(s_studio_page, -1), (void *)1003);
    lv_obj_move_foreground(s_studio_tools);
    lv_obj_set_hidden(s_studio_page, true);
}

static void close_library(lv_event_t *event)
{
    (void)event;
    lv_obj_set_hidden(s_library, true);
    sync_master();
}
static void choose_track(lv_event_t *event)
{
    int track = (int)(intptr_t)lv_event_get_user_data(event);
    solco_select(s_library_deck, track);
    lv_obj_set_hidden(s_library, true);
    sync_master();
    s_first = true;
}
static void open_library(int deck)
{
    solco_status_t status;
    solco_status(&status);
    if (status.playing[deck] || status.scratching[deck])
    {
        solco_next(deck); /* Existing audible-deck guard supplies the visible error. */
        return;
    }
    s_library_deck = deck;
    lv_label_set_text_fmt(s_library_name, "LIBRERIA  %c", 'A' + deck);
    lv_obj_clean(s_library_list);
    int count = solco_library_count();
    for (int i = 0; i < count; ++i)
    {
        char title[80];
        solco_library_title(i, title, sizeof(title));
        lv_obj_t *item = lv_button_create(s_library_list);
        lv_obj_set_width(item, lv_pct(100));
        lv_obj_set_style_pad_all(item, 8, 0);
        lv_obj_t *text = lv_label_create(item);
        lv_label_set_text(text, title);
        lv_obj_set_width(text, lv_pct(100));
        lv_obj_center(text);
        lv_obj_set_height(item, y(48));
        lv_obj_set_style_bg_color(item, lv_color_hex(0x14202D), 0);
        lv_obj_set_style_text_color(item, lv_color_hex(0xE5F8F4), 0);
        lv_obj_set_style_text_font(item, s_landscape ? &lv_font_montserrat_16 : &lv_font_montserrat_14, 0);
        lv_obj_set_height(text, y(20));
        lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
        lv_obj_add_event_cb(item, choose_track, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_set_hidden(s_library, false);
    s_volume_open = false;
    sync_master();
}

static void sync_master(void)
{
    bool detail =
        (!lv_obj_is_hidden(s_detail) || s_studio_view >= 0) && lv_obj_is_hidden(s_library);
    lv_obj_set_hidden(s_master, detail && !s_volume_open);
    lv_obj_set_clickable(s_master, detail && s_volume_open);
    lv_obj_set_style_bg_opa(s_master, detail && s_volume_open ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    s_first = true;
}
static void detail_volume(lv_event_t *event)
{
    (void)event;
    solco_scratch(s_deck, 2, 0);
    s_disc_drag = false;
    s_volume_open = !s_volume_open;
    update_wheel_mode();
    sync_master();
}
static void update_wheel_mode(void)
{
    uint32_t accent = s_deck == 0 ? 0x69E4CE : 0xFFBA78;
    lv_obj_set_style_bg_color(s_record_label, lv_color_hex(accent), 0);
    if (s_wheel_mode)
    {
        lv_label_set_text_fmt(s_wheel_mode, "DECK %c\nMENU", 'A' + s_deck);
    }
}
static void wheel_mode(lv_event_t *event)
{
    (void)event;
    solco_scratch(s_deck, 2, 0);
    s_disc_drag = false;
    s_wheel_vinyl = !s_wheel_vinyl;
    update_wheel_mode();
    s_first = true;
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    studio_refresh();
    solco_status_t status;
    solco_status(&status);
    if(s_quick_play) lv_label_set_text(s_quick_play,status.playing[s_deck]?LV_SYMBOL_PAUSE:LV_SYMBOL_PLAY);
    for (int i = 0; i < 2; ++i)
    {
        if (s_first || strcmp(status.title[i], s_previous.title[i]))
        {
            lv_label_set_text(s_title[i], status.title[i]);
        }
        unsigned seconds = status.frames[i] / 44100;
        if (s_first || seconds != s_previous.frames[i] / 44100 ||
            status.preparing[i] != s_previous.preparing[i] ||
            status.prepare_percent[i] != s_previous.prepare_percent[i])
        {
            if (status.preparing[i])
            {
                lv_label_set_text_fmt(s_time[i], "%d%%", status.prepare_percent[i]);
            }
            else
            {
                lv_label_set_text_fmt(s_time[i], "%02u:%02u", seconds / 60, seconds % 60);
            }
        }
        if (s_first || status.ready[i] != s_previous.ready[i])
        {
            lv_obj_set_state(lv_obj_get_parent(s_play[i]), LV_STATE_DISABLED, !status.ready[i]);
        }
        if (s_first || status.playing[i] != s_previous.playing[i])
        {
            lv_label_set_text(s_play[i], status.playing[i] ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
            lv_obj_set_style_bg_color(lv_obj_get_parent(s_play[i]),
                                      lv_color_hex(i == 0
                                                       ? (status.playing[i] ? 0x269E88 : 0x245E58)
                                                       : (status.playing[i] ? 0xA96C39 : 0x684A34)),
                                      0);
        }
        if (s_first || memcmp(s_wave[i], status.wave[i], sizeof(s_wave[i])) ||
            status.playing[i] != s_wave_active[i])
        {
            memcpy(s_wave[i], status.wave[i], sizeof(s_wave[i]));
            s_wave_active[i] = status.playing[i];
            lv_obj_invalidate(s_wave_obj[i]);
        }
    }
    if (!lv_obj_is_hidden(s_detail))
    {
        float rate = status.rate[s_deck] / 65536.f;
        if (s_first || status.rate[s_deck] != s_previous.rate[s_deck])
            lv_label_set_text_fmt(s_rate_text, "DECK %c / %s %.1fx", 'A' + s_deck,
                                  rate < 0 ? "INDIETRO" : "AVANTI", fabsf(rate));
        lv_label_set_text(s_loop_toggle, status.looping[s_deck] ? "EXIT" : "LOOP");
        if ((int32_t)(s_loop_error_until - lv_tick_get()) > 0)
            lv_label_set_text(s_loop_text, "Intervallo non valido: IN prima di OUT");
        else
            lv_label_set_text_fmt(s_loop_text,
                                  status.loop_out[s_deck] > status.loop_in[s_deck]
                                      ? "LOOP %.2f s %s"
                                  : status.loop_marked[s_deck] ? "IN OK / premi OUT"
                                                               : "IN / OUT per creare un loop",
                                  (status.loop_out[s_deck] - status.loop_in[s_deck]) / 44100.f,
                                  status.looping[s_deck] ? "ON" : "OFF");
        if (s_first || strcmp(status.title[s_deck], s_previous.title[s_deck]))
        {
            lv_label_set_text(s_detail_title, status.title[s_deck]);
        }
        if (s_first || status.keylock[s_deck] != s_previous.keylock[s_deck])
        {
            lv_label_set_text(s_keylock_button,
                              status.keylock[s_deck] ? "TONO FISSO: ON" : "TONO FISSO: OFF");
        }
        if (s_first || status.scratching[s_deck] != s_previous.scratching[s_deck])
        {
            lv_label_set_text(s_hint, status.scratching[s_deck] ? "SCRATCH"
                                      : board_info()->has_knob && s_wheel_vinyl
                                          ? "Gira la ghiera o il disco"
                                          : "Trascina il disco");
        }
        if (s_first || status.ready[s_deck] != s_previous.ready[s_deck])
        {
            lv_obj_set_state(lv_obj_get_parent(s_detail_play), LV_STATE_DISABLED,
                             !status.ready[s_deck]);
        }
        unsigned seconds = status.frames[s_deck] / 44100;
        if (s_first || seconds != s_previous.frames[s_deck] / 44100 ||
            status.preparing[s_deck] != s_previous.preparing[s_deck] ||
            status.prepare_percent[s_deck] != s_previous.prepare_percent[s_deck])
        {
            if (status.preparing[s_deck])
            {
                lv_label_set_text_fmt(s_detail_time, "MP3  %d%%", status.prepare_percent[s_deck]);
            }
            else
            {
                lv_label_set_text_fmt(s_detail_time,
                                      board_info()->round ? "%c %02u:%02u" : "DECK %c  %02u:%02u",
                                      'A' + s_deck, seconds / 60, seconds % 60);
            }
        }
        if (s_first || status.playing[s_deck] != s_previous.playing[s_deck])
        {
            lv_label_set_text(s_detail_play,
                              status.playing[s_deck] ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
        if (s_first || status.platter_phase[s_deck] != s_previous.platter_phase[s_deck])
        {
            invalidate_marker(s_disc_angle);
            s_disc_angle = status.platter_phase[s_deck] * (6.2831853f * 10 / (44100 * 18));
            invalidate_marker(s_disc_angle);
        }
    }
    int percent = (status.cross + 5) / 10;
    if (s_first || status.cross != s_previous.cross)
    {
        lv_label_set_text_fmt(s_cross_text, "A  %d : %d  B", 100 - percent, percent);
    }
    if (s_first || status.volume != s_previous.volume)
    {
        lv_label_set_text_fmt(s_volume_text, "VOLUME  %d%%", status.volume);
        lv_slider_set_value(s_volume, status.volume, LV_ANIM_OFF);
        lv_label_set_text(s_mute, status.volume == 0 ? "RIATTIVA" : "MUTE");
        lv_label_set_text_fmt(s_detail_volume, s_volume_open ? "CHIUDI" : "VOL %d", status.volume);
    }
    int level = status.peak ? 600 + (int)(200 * log10f(status.peak / 32768.0f)) : 0;
    lv_bar_set_value(s_meter, level < 0 ? 0 : level, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_meter,
                              lv_color_hex(level >= 590   ? 0xFF626B
                                           : level >= 540 ? 0xFFBA78
                                                          : 0x69E4CE),
                              LV_PART_INDICATOR);
    if (s_first || strcmp(status.error, s_previous.error) ||
        status.underruns != s_previous.underruns)
    {
        lv_obj_set_hidden(s_info, !status.error[0] && !status.underruns);
        if (status.error[0])
        {
            lv_label_set_text(s_info, status.error);
            lv_obj_set_style_text_color(s_info, lv_color_hex(0xFFBA78), 0);
        }
        else if (status.underruns)
        {
            lv_label_set_text_fmt(s_info, "BUFFER / %lu", (unsigned long)status.underruns);
        }
        else
        {
            lv_label_set_text(s_info,
                              SOLCO_OUTPUT_CHANNELS == 1 ? "44.1 kHz / MONO" : "44.1 kHz / STEREO");
            lv_obj_set_style_text_color(s_info, lv_color_hex(0x8FA7BF), 0);
        }
    }
    s_previous = status;
    s_first = false;
}

/* Development commands never change the user's master volume. */
bool devcon_app_command(char *line)
{
#if CONFIG_WATCH_DEV_CONSOLE
    int type, value, velocity;
    if(!strcmp(line,"solco-layout")) {
        lv_port_lock();
        printf("@LAYOUT width=%d height=%d landscape=%d studio=%d deck=%d detail=%d library=%d\n",
               s_width,s_height,s_landscape,s_studio_view,s_deck,!lv_obj_is_hidden(s_detail),!lv_obj_is_hidden(s_library));
        lv_port_unlock();printf("@OK solco-layout\n");return true;
    }
    if (sscanf(line, "studio-motion %d", &value) == 1)
    {
        solco_motion_mode(value);
        printf("@OK studio-motion\n");
        return true;
    }
    if (sscanf(line, "studio-event %d %d %d", &type, &value, &velocity) == 3)
    {
        solco_studio_command(type, value, velocity);
        printf("@OK studio-event\n");
        return true;
    }
    if (sscanf(line, "studio-view %d", &value) == 1 && value >= -1 && value <= 3)
    {
        lv_port_lock();
        studio_open(value);
        lv_port_unlock();
        printf("@OK studio-view\n");
        return true;
    }
    if (!strcmp(line, "studio-saved"))
    {
        printf("@SAVED match=%d\n", solco_studio_saved());
        printf("@OK studio-saved\n");
        return true;
    }
    if (!strcmp(line, "studio-burst"))
    {
        for (int bank = 0; bank < 32; bank++)
        {
            solco_studio_command(STUDIO_HIT, bank, 127);
        }
        printf("@OK studio-burst\n");
        return true;
    }
    int deck, action;
    if (sscanf(line, "deck-keylock %d %d", &deck, &action) == 2)
    {
        printf("@KEYLOCK accepted=%d\n", solco_keylock(deck, action != 0));
        printf("@OK deck-keylock\n");
        return true;
    }
    if (sscanf(line, "deck-rate %d %d", &deck, &action) == 2)
    {
        printf("@RATE accepted=%d\n", solco_rate(deck, action));
        printf("@OK deck-rate\n");
        return true;
    }
    if (sscanf(line, "deck-loop %d %d", &deck, &action) == 2)
    {
        printf("@LOOP accepted=%d\n", solco_loop(deck, action));
        printf("@OK deck-loop\n");
        return true;
    }
    if (sscanf(line, "deck-open %d", &deck) == 1 && deck >= 0 && deck < 2)
    {
        lv_port_lock();
        studio_open(-1);
        s_deck = deck;
        lv_obj_set_hidden(s_brand, true);
        lv_obj_set_hidden(s_mixer, true);
        lv_obj_set_hidden(s_detail, false);
        lv_obj_set_hidden(s_deck_menu, true);
        update_wheel_mode();
        sync_master();
        lv_port_unlock();
        printf("@OK deck-open\n");
        return true;
    }
    if (!strcmp(line, "studio"))
    {
        studio_status_t state;
        solco_studio_status(&state);
        printf("@STUDIO init_us=%lu kit=%d bpm=%d play=%d rec=%d step=%d voices=%d hits=%lu "
               "drop=%lu late=%lu "
               "frame=%llu "
               "revision=%lu pattern=%u/%u/%u/%u/%u/%u/%u/%u\n",
               (unsigned long)solco_studio_init_us(), state.kit, state.bpm, state.playing,
               state.recording, state.step, state.voices, (unsigned long)state.hits,
               (unsigned long)state.dropped, (unsigned long)state.late,
               (unsigned long long)state.frame, (unsigned long)state.revision,
               state.pattern.pattern[0], state.pattern.pattern[1], state.pattern.pattern[2],
               state.pattern.pattern[3], state.pattern.pattern[4], state.pattern.pattern[5],
               state.pattern.pattern[6], state.pattern.pattern[7]);
        printf("@LINK transport=off audio=local\n");
        lv_port_lock();
        int view = s_studio_view;
        lv_port_unlock();
        printf("@VIEW studio=%d\n", view);
        lv_port_lock();
        bool menu = !lv_obj_is_hidden(s_deck_menu), tools = !lv_obj_is_hidden(s_studio_tools);
        lv_port_unlock();
        printf("@UI deck_menu=%d studio_tools=%d\n", menu, tools);
        uint32_t reads, errors, hits;
        solco_motion_stats(&reads, &errors, &hits);
        int learned, selected_pad;
        solco_motion_state(&learned, &selected_pad);
        printf("@GESTURE learned=%d pad=%d\n", learned, selected_pad);
        printf("@MOTION supported=%d mode=%d reads=%lu errors=%lu hits=%lu\n", imu_supported(),
               solco_motion_get(), (unsigned long)reads, (unsigned long)errors,
               (unsigned long)hits);
        printf("@OK studio\n");
        return true;
    }
    if (!strcmp(line, "solco-load-stress"))
    {
        printf(solco_test_load_stress() ? "@OK solco-load-stress\n" : "@ERR queue busy\n");
        return true;
    }
    if (!strcmp(line, "solco-mp3-reset"))
    {
        printf(solco_test_mp3_reset() ? "@OK solco-mp3-reset\n" : "@ERR fixture repair failed\n");
        return true;
    }
    if (!strcmp(line, "solco-mp3-check"))
    {
        solco_test_mp3_verify();
        printf("@OK solco-mp3-check\n");
        return true;
    }
    if (!strcmp(line, "solco-mp3-fixture"))
    {
        printf(solco_test_mp3_fixture() ? "@OK solco-mp3-fixture\n"
                                        : "@ERR fixture exists or SD unavailable\n");
        return true;
    }
    if (!strcmp(line, "solco-load-mp3"))
    {
        printf(solco_test_load_mp3() ? "@OK solco-load-mp3\n" : "@ERR queue busy\n");
        return true;
    }
    if (!strcmp(line, "solco-fixture"))
    {
        printf(solco_test_fixture() ? "@OK solco-fixture\n"
                                    : "@ERR fixture not created (exists or SD unavailable)\n");
        return true;
    }
    if (!strcmp(line, "solco-load-test"))
    {
        printf(solco_test_load() ? "@OK solco-load-test\n" : "@ERR queue busy\n");
        return true;
    }
#endif
    if (strcmp(line, "solco"))
    {
        return false;
    }
    solco_status_t status;
    solco_status(&status);
    printf("@SOLCO volume=%d cross=%d playing=%d/%d peak=%u empty=%lu errors=%lu clips=%lu "
           "launcher=%d\n",
           status.volume, status.cross, status.playing[0], status.playing[1], status.peak,
           (unsigned long)status.underruns, (unsigned long)status.audio_errors,
           (unsigned long)status.clips, solco_launcher_state());
    printf("@VINYL pos=%lu/%lu speed=%ld/%ld held=%d/%d ready=%d/%d mix_us=%lu\n",
           (unsigned long)status.frames[0], (unsigned long)status.frames[1], (long)status.speed[0],
           (long)status.speed[1], status.scratching[0], status.scratching[1], status.ready[0],
           status.ready[1], (unsigned long)status.mix_us);
    printf("@PREP busy=%d/%d percent=%d/%d tracks=%d\n", status.preparing[0], status.preparing[1],
           status.prepare_percent[0], status.prepare_percent[1], solco_library_count());
    printf("@CACHE miss=%lu/%lu tag=%lu/%lu wheel=%d\n", (unsigned long)status.miss_frame[0],
           (unsigned long)status.miss_frame[1], (unsigned long)status.miss_tag[0],
           (unsigned long)status.miss_tag[1], s_wheel_vinyl);
    printf("@TIMING cycle_us=%lu late=%lu blocks=%lu dma_frames=128 dma_count=6\n",
           (unsigned long)status.cycle_us, (unsigned long)status.late_cycles,
           (unsigned long)status.blocks);
    if (status.error[0])
    {
        printf("@ERROR %s\n", status.error);
    }
    printf("@KEYLOCK enabled=%d/%d\n", status.keylock[0], status.keylock[1]);
    printf("@RATE value=%ld/%ld\n", (long)status.rate[0], (long)status.rate[1]);
    printf("@LOOPS active=%d/%d in=%lu/%lu out=%lu/%lu\n", status.looping[0], status.looping[1],
           (unsigned long)status.loop_in[0], (unsigned long)status.loop_in[1],
           (unsigned long)status.loop_out[0], (unsigned long)status.loop_out[1]);
    printf("@OK solco\n");
    return true;
}

static bool home_back(void)
{
    if (s_studio_view >= 0)
    {
        studio_open(-1);
        return true;
    }
    extapp_return_to_launcher();
    return true;
}

static void create_ui(void)
{
    s_width = lv_display_get_horizontal_resolution(lv_port_display());
    s_height = lv_display_get_vertical_resolution(lv_port_display());
    s_layout=solco_layout_for(s_width,s_height,board_info()->round);
    s_landscape=s_layout.landscape;
    bool round = board_info()->round;
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, s_width, s_height);
    lv_obj_set_style_bg_color(home, lv_color_hex(0x080E16), 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(home, lv_color_white(), 0);
    lv_obj_set_scrollable(home, false);
    lv_obj_t *name = label(home, "S O L C O", 95, 8, 170, 0xE5F8F4);
    s_brand = name;
    lv_obj_set_clickable(name, true);
    lv_obj_add_event_cb(name, studio_launch, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(name,s_landscape ? "STUDIO >" : "SOLCO +");
    lv_obj_set_y(name, y(round ? 16 : 4));
    lv_obj_set_height(name, y(34));
    lv_obj_set_style_bg_color(name, lv_color_hex(0x243747), 0);
    lv_obj_set_style_bg_opa(name, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(name, 12, 0);
    lv_obj_set_style_pad_top(name, y(3), 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
    s_mixer = container(home);
    /* Let header touches reach SOLCO + behind the transparent mixer panel. */
    lv_obj_set_clickable(s_mixer, false);

    for (int i = 0; i < 2; ++i)
    {
        solco_rect_t box=s_layout.deck[i];
        int width=box.w;
        uint32_t color = i == 0 ? 0x69E4CE : 0xFFBA78;
        lv_obj_t *card = lv_obj_create(s_mixer);
        lv_obj_remove_style_all(card);
        lv_obj_set_pos(card,x(box.x),y(box.y));
        lv_obj_set_size(card,x(box.w),y(box.h));
        lv_obj_set_scrollable(card, false);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x14202D), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x25384A), 0);
        s_title[i] = label(card, "Caricamento...", 12, 8, s_landscape ? width-24 : width - 92, color);
        lv_obj_set_height(s_title[i], y(18));
        lv_obj_t *open = control(card, i == 0 ? "A  >" : "B  >", s_landscape ? 10 : width - 72, s_landscape ? 33 : 4, 66, show_deck, i);
        lv_obj_set_height(lv_obj_get_parent(open), y(32));
        s_wave_obj[i] = lv_obj_create(card);
        lv_obj_remove_style_all(s_wave_obj[i]);
        lv_obj_set_pos(s_wave_obj[i], x(12), y(s_landscape ? 76 : round ? 25 : 29));
        lv_obj_set_size(s_wave_obj[i], x(width - 24), y(s_landscape ? 30 : round ? 11 : 14));
        lv_obj_set_clickable(s_wave_obj[i], false);
        lv_obj_set_scrollable(s_wave_obj[i], false);
        lv_obj_add_event_cb(s_wave_obj[i], draw_wave, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)i);
        s_time[i] = label(card, "00:00", s_landscape ? 89 : 12, s_landscape ? 40 : round ? 55 : 61, s_landscape ? 70 : 56, color);
        int bw = s_landscape ? 48 : (width - 90) / 3;
        s_play[i] = button(card, LV_SYMBOL_PLAY, s_landscape ? 8 : 76, s_landscape ? 119 : round ? 40 : 46, bw,
                           i == 0 ? 0x247E71 : 0x986035, i * 3);
        button(card, "CUE", s_landscape ? 60 : 80 + bw, s_landscape ? 119 : round ? 40 : 46, bw, 0x243747, i * 3 + 1);
        button(card, "FILE", s_landscape ? 112 : 84 + bw * 2, s_landscape ? 119 : round ? 40 : 46, bw, 0x243747, i * 3 + 2);
    }
    s_cross_text = label(s_mixer, "A  50 : 50  B", 85, 232, 190, 0xDCE6F0);
    lv_obj_set_style_text_align(s_cross_text, LV_TEXT_ALIGN_CENTER, 0);
    s_cross = slider(s_mixer, 258, 1000, 500, cross_event);
    lv_obj_set_x(s_cross, x(round ? 58 : 28));
    lv_obj_set_width(s_cross, x(round ? 244 : 304));

    /* Immersive platter: master controls become an optional bottom drawer. */
    s_detail = container(home);
    lv_obj_set_height(s_detail, s_height);
    s_disc = lv_obj_create(s_detail);
    lv_obj_remove_style_all(s_disc);
    int diameter=s_layout.disc.w;
    lv_obj_set_pos(s_disc,s_layout.disc.x,s_layout.disc.y);
    lv_obj_set_size(s_disc, diameter, diameter);
    lv_obj_set_style_radius(s_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_disc, lv_color_hex(0x0C151F), 0);
    lv_obj_set_style_bg_opa(s_disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_disc, 2, 0);
    lv_obj_set_style_border_color(s_disc, lv_color_hex(0x35485B), 0);
    lv_obj_set_scrollable(s_disc, false);
    lv_obj_set_clickable(s_disc, true);
    lv_obj_add_event_cb(s_disc, platter_touch, LV_EVENT_ALL, NULL);
    create_grooves(diameter);
    if(s_landscape) {
        control(s_detail,"< MIX",8,8,64,show_mixer,0);
        control(s_detail,"MENU",288,8,64,deck_menu,0);
        s_quick_play=control(s_detail,LV_SYMBOL_PLAY,8,310,64,detail_action,0);
        control(s_detail,"CUE",288,310,64,detail_action,1);
    }
    s_record_label = lv_button_create(s_disc);
    lv_obj_remove_style_all(s_record_label);
    lv_obj_set_size(s_record_label, diameter * 32 / 100, diameter * 32 / 100);
    lv_obj_center(s_record_label);
    lv_obj_set_style_radius(s_record_label, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_record_label, lv_color_hex(0x69E4CE), 0);
    lv_obj_set_style_bg_opa(s_record_label, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_record_label, false);
    lv_obj_set_clickable(s_record_label, true);
    lv_obj_add_event_cb(s_record_label, deck_menu, LV_EVENT_CLICKED, NULL);
    lv_obj_t *print = lv_label_create(s_record_label);
    lv_label_set_text(print, "DECK A\nMENU");
    lv_obj_set_style_text_align(print, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(print, lv_color_hex(0x08131A), 0);
    lv_obj_set_style_text_font(print, s_landscape ? &lv_font_montserrat_16 : &lv_font_montserrat_14, 0);
    lv_obj_center(print);
    s_wheel_mode = print;
    lv_obj_add_event_cb(s_disc, draw_disc, LV_EVENT_DRAW_POST, NULL);
    s_deck_menu = container(s_detail);
    lv_obj_set_height(s_deck_menu, s_height);
    lv_obj_set_style_bg_color(s_deck_menu, lv_color_hex(0x101C28), 0);
    lv_obj_set_style_bg_opa(s_deck_menu, LV_OPA_COVER, 0);
    control(s_deck_menu, "< MIX", 62, 32, 104, show_mixer, 0);
    control(s_deck_menu, "VINILE", 180, 32, 118, deck_menu, 0);
    s_detail_time = label(s_deck_menu, "A 00:00", 48, 84, 128, 0xE5F8F4);
    lv_obj_t *rate_button = control(s_deck_menu, "VELOCITA", 184, 78, 136, rate_action, 0);
    lv_obj_set_height(lv_obj_get_parent(rate_button), y(30));
    s_detail_title = label(s_deck_menu, "", 40, 111, 280, 0xAEC3D4);
    s_detail_play = control(s_deck_menu, LV_SYMBOL_PLAY, 40, 132, 88, detail_action, 0);
    control(s_deck_menu, "CUE", 136, 132, 88, detail_action, 1);
    control(s_deck_menu, "FILE", 232, 132, 88, detail_action, 2);
    control(s_deck_menu, "IN", 40, 184, 88, loop_action, 0);
    control(s_deck_menu, "OUT", 136, 184, 88, loop_action, 1);
    s_loop_toggle = control(s_deck_menu, "LOOP", 232, 184, 88, loop_action, 2);
    control(s_deck_menu, "1/2", 56, 236, 76, loop_action, 3);
    control(s_deck_menu, "x2", 142, 236, 76, loop_action, 4);
    s_detail_volume = control(s_deck_menu, "VOL", 228, 236, 76, detail_volume, 0);
    s_loop_text = label(s_deck_menu, "IN / OUT per creare un loop", 60, 285, 250, 0x8FA7BF);
    s_hint = label(s_deck_menu, "", 80, 308, 210, 0x8FA7BF);
    if (board_info()->has_knob)
    {
        control(s_deck_menu, "GHIERA: VINILE/VOL", 84, 304, 192, wheel_mode, 0);
        lv_obj_set_hidden(s_hint, true);
    }
    s_rate_panel = container(s_deck_menu);
    lv_obj_set_height(s_rate_panel, s_height);
    lv_obj_set_style_bg_color(s_rate_panel, lv_color_hex(0x101C28), 0);
    lv_obj_set_style_bg_opa(s_rate_panel, LV_OPA_COVER, 0);
    control(s_rate_panel, "< MENU", 62, 32, 104, rate_action, 0);
    control(s_rate_panel, "VINILE", 180, 32, 118, deck_menu, 0);
    s_rate_text = label(s_rate_panel, "AVANTI 1x", 60, 88, 260, 0xE5F8F4);
    control(s_rate_panel, "INDIETRO 2x", 40, 112, 136, rate_action, -131072);
    control(s_rate_panel, "AVANTI 2x", 184, 112, 136, rate_action, 131072);
    control(s_rate_panel, "INDIETRO 1.5x", 40, 160, 136, rate_action, -98304);
    control(s_rate_panel, "AVANTI 1.5x", 184, 160, 136, rate_action, 98304);
    control(s_rate_panel, "INDIETRO 1x", 40, 208, 136, rate_action, -65536);
    control(s_rate_panel, "AVANTI 1x", 184, 208, 136, rate_action, 65536);
    control(s_rate_panel, "INDIETRO 0.5x", 40, 256, 136, rate_action, -32768);
    control(s_rate_panel, "AVANTI 0.5x", 184, 256, 136, rate_action, 32768);
    s_keylock_button = control(s_rate_panel, "TONO FISSO: OFF", 84, 302, 192, keylock_action, 0);
    lv_obj_set_hidden(s_rate_panel, true);
    lv_obj_set_hidden(s_deck_menu, true);
    lv_obj_set_hidden(s_detail, true);

    s_master = container(home);
    lv_obj_set_pos(s_master, 0, y(260));
    lv_obj_set_size(s_master, s_width, s_height - y(260));
    lv_obj_set_clickable(s_master, false);
    lv_obj_set_style_bg_color(s_master, lv_color_hex(0x080E16), 0);
    s_volume_text = label(s_master, "VOLUME  0%", 105, 19, 150, 0xE5F8F4);
    lv_obj_set_style_text_align(s_volume_text, LV_TEXT_ALIGN_CENTER, 0);
    s_volume = slider(s_master, 49, 100, 0, volume_event);
    lv_obj_set_x(s_volume, x(98));
    lv_obj_set_width(s_volume, x(164));
    control(s_master, "-", round ? 43 : 14, 21, 44, volume_step, -1);
    control(s_master, "+", round ? 273 : 302, 21, 44, volume_step, 1);
    s_mute = control(s_master, "RIATTIVA", 126, 68, 108, volume_step, 0);
    lv_obj_set_height(lv_obj_get_parent(s_mute), y(27));
    s_meter = lv_bar_create(s_master);
    lv_bar_set_range(s_meter, 0, 600);
    lv_obj_set_pos(s_meter, x(110), y(13));
    lv_obj_set_size(s_meter, x(140), y(3));
    lv_obj_set_style_bg_color(s_meter, lv_color_hex(0x273341), 0);
    lv_obj_set_style_bg_color(s_meter, lv_color_hex(0x69E4CE), LV_PART_INDICATOR);
    s_info = label(home, "", 60, 31, 240, 0x8FA7BF);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_12, 0);
    lv_obj_set_hidden(s_info, true);
    s_library = container(home);
    lv_obj_set_style_bg_color(s_library, lv_color_hex(0x080E16), 0);
    lv_obj_set_style_bg_opa(s_library, LV_OPA_COVER, 0);
    s_library_name = label(s_library, "LIBRERIA A", 75, 12, 210, 0xE5F8F4);
    lv_obj_set_style_text_align(s_library_name, LV_TEXT_ALIGN_CENTER, 0);
    control(s_library, "< TORNA", round ? 50 : 12, 43, 100, close_library, 0);
    label(s_library, "WAV / MP3", 175, 55, 130, 0x8FA7BF);
    s_library_list = lv_obj_create(s_library);
    lv_obj_set_flex_flow(s_library_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_library_list, LV_DIR_VER);
    lv_obj_set_style_pad_row(s_library_list, y(4), 0);
    lv_obj_set_pos(s_library_list, x(round ? 40 : 12), y(91));
    lv_obj_set_size(s_library_list, x(round ? 280 : 336), y(178));
    lv_obj_set_style_bg_color(s_library_list, lv_color_hex(0x080E16), 0);
    lv_obj_set_style_border_width(s_library_list, 0, 0);
    lv_obj_set_style_pad_all(s_library_list, 0, 0);
    lv_obj_set_hidden(s_library, true);
    create_studio(home);
    lv_obj_move_foreground(s_master);
    lv_obj_add_event_cb(home, knob_event, lv_port_knob_event(), NULL);
    app_manager_set_home(home, home_back);
    lv_timer_create(refresh, 50, NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(extapp_sdk_init());
    sys_check_last_reset();
    sys_worker_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    /* Solco uses the long edge horizontally; launcher orientation is untouched. */
    bool rectangle=!board_info()->round && board_info()->width!=board_info()->height;
    ESP_ERROR_CHECK(lv_port_init_orientation(rectangle && board_info()->width<board_info()->height ? 1 : 0));
    clock_init();
    solco_engine_sync_init();
    lv_port_lock();
    ui_init();
    create_ui();
    lv_port_unlock();
    app_manager_init();
    ESP_ERROR_CHECK(power_init());
    power_keep_screen_on(true);
    ESP_ERROR_CHECK(solco_engine_start());
    devcon_start();
    sys_post(solco_install_launcher, NULL);
    sys_heap_log("solco ready");
    ESP_LOGI("solco", "Ready: %s; dual-deck PCM16 44100 Hz; %d ch; volume 0%%", board_info()->name,
             SOLCO_OUTPUT_CHANNELS);
}
