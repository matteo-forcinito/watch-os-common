/* Astri: the screen and the gestures. See astri.h. */
#include "astri.h"

#include "core/settings.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "extapp_sdk.h"
#include "lvgl.h"
#include "ui/ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define FRAME_MS      60      /* redraw at most ~16 times a second while turning */
#define LIVE_MS       30000   /* the real sky moves slowly: refresh twice a minute */
#define TAP_PX        10      /* a touch that moves less is a tap */
#define CENTRE_PX     40      /* a tap here goes back to "now" */
#define NAME_MS       3000
#define MINUTES_PER_TURN (24 * 60)

typedef struct
{
    lv_obj_t *canvas;
    lv_obj_t *when;           /* date and time shown */
    lv_obj_t *offset;         /* "+3 h 20" while away from now */
    lv_obj_t *name;           /* what was tapped */
    lv_timer_t *frame;
    astri_view_t view;
    int32_t offset_min;       /* minutes away from now */
    float press_angle;
    int32_t press_offset;
    lv_point_t press_point;
    bool moved, dirty, fast, waiting_clock;
    uint32_t last_render, name_until;
    float lat, lon;
} astri_ui_t;

static astri_ui_t *s_ui;

static float angle_of(const astri_ui_t *ui, lv_point_t p)
{
    return atan2f((float)(p.y - ui->view.cy), (float)(p.x - ui->view.cx)) * (180.0f / (float)M_PI);
}

static void update_texts(astri_ui_t *ui, time_t t)
{
    static const char *const days[] = {"Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab"};
    static const char *const months[] = {"gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic"};
    struct tm tm;
    localtime_r(&t, &tm);
    char text[48];
    snprintf(text, sizeof(text), "%s %d %s  %02d:%02d", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon], tm.tm_hour,
             tm.tm_min);
    lv_label_set_text(ui->when, text);

    if (ui->offset_min == 0)
    {
        lv_obj_add_flag(ui->offset, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int32_t m = ui->offset_min < 0 ? -ui->offset_min : ui->offset_min;
    if (m >= 24 * 60) snprintf(text, sizeof(text), "%c%ld g %ld h", ui->offset_min < 0 ? '-' : '+', (long)(m / 1440), (long)(m % 1440 / 60));
    else snprintf(text, sizeof(text), "%c%ld h %02ld", ui->offset_min < 0 ? '-' : '+', (long)(m / 60), (long)(m % 60));
    lv_label_set_text(ui->offset, text);
    lv_obj_remove_flag(ui->offset, LV_OBJ_FLAG_HIDDEN);
}

static void render(astri_ui_t *ui)
{
    time_t t = time(NULL) + (time_t)ui->offset_min * 60;
    astri_sky_render(&ui->view, ui->canvas, t, ui->lat, ui->lon, ui->fast);
    lv_obj_invalidate(ui->canvas);
    update_texts(ui, t);
    ui->dirty = false;
    ui->last_render = lv_tick_get();
}

#define CLOCK_VALID 1735689600   /* 1 Jan 2025: earlier means the clock is not set yet */

static void on_frame(lv_timer_t *timer)
{
    astri_ui_t *ui = lv_timer_get_user_data(timer);
    if (ui->waiting_clock)
    {
        if (time(NULL) < CLOCK_VALID)
        {
            return;   /* no sky of 1970: wait for NTP or the phone */
        }
        ui->waiting_clock = false;
        lv_obj_add_flag(ui->name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->canvas, LV_OBJ_FLAG_HIDDEN);
        ui->dirty = true;
    }
    if (ui->name_until && lv_tick_elaps(ui->name_until) < 0x80000000u)
    {
        lv_obj_add_flag(ui->name, LV_OBJ_FLAG_HIDDEN);
        ui->name_until = 0;
    }
    if (ui->dirty || lv_tick_elaps(ui->last_render) >= LIVE_MS)
    {
        render(ui);
    }
}

static void show_name(astri_ui_t *ui, const char *name)
{
    lv_label_set_text(ui->name, name);
    lv_obj_remove_flag(ui->name, LV_OBJ_FLAG_HIDDEN);
    ui->name_until = lv_tick_get() + NAME_MS;
}

static void on_touch(lv_event_t *e)
{
    astri_ui_t *ui = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);

    if (code == LV_EVENT_PRESSED)
    {
        ui->press_point = p;
        ui->press_angle = angle_of(ui, p);
        ui->press_offset = ui->offset_min;
        ui->moved = false;
    }
    else if (code == LV_EVENT_PRESSING)
    {
        int dx = p.x - ui->press_point.x, dy = p.y - ui->press_point.y;
        int from_centre = (p.x - ui->view.cx) * (p.x - ui->view.cx) + (p.y - ui->view.cy) * (p.y - ui->view.cy);
        if (!ui->moved && dx * dx + dy * dy < TAP_PX * TAP_PX)
        {
            return;
        }
        if (from_centre < CENTRE_PX * CENTRE_PX)
        {
            return;   /* the angle is meaningless near the centre */
        }
        ui->moved = true;
        /* Clockwise on screen = forward in time; one full turn = one day. Unwrapped across +-180. */
        float delta = angle_of(ui, p) - ui->press_angle;
        while (delta > 180.0f) delta -= 360.0f;
        while (delta < -180.0f) delta += 360.0f;
        ui->press_angle += delta;
        ui->press_offset += (int32_t)lroundf(delta * MINUTES_PER_TURN / 360.0f);
        if (ui->press_offset != ui->offset_min)
        {
            ui->offset_min = ui->press_offset;
            ui->fast = true;
            ui->dirty = true;
        }
    }
    else if (code == LV_EVENT_RELEASED)
    {
        if (ui->moved)
        {
            ui->fast = false;   /* settle with the Milky Way and the names */
            ui->dirty = true;
            return;
        }
        int dx = p.x - ui->view.cx, dy = p.y - ui->view.cy;
        if (dx * dx + dy * dy < CENTRE_PX * CENTRE_PX)
        {
            if (ui->offset_min != 0)
            {
                ui->offset_min = 0;
                ui->dirty = true;
                show_name(ui, "Adesso");
            }
            return;
        }
        char name[32];
        if (astri_sky_pick(p.x, p.y, 22, name, sizeof(name)) != NULL)
        {
            show_name(ui, name);
        }
    }
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, lv_align_t align, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_align(label, align, 0, y);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(label, "");
    return label;
}

void astri_ui_create(void *screen_ptr)
{
    lv_obj_t *screen = screen_ptr;
    astri_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    s_ui = ui;
    const board_info_t *board = ui_board_info();
    ui->view.width = board->width;
    ui->view.height = board->height;
    ui->view.cx = board->width / 2;
    ui->view.cy = board->height / 2;
    ui->view.radius = (board->width < board->height ? board->width : board->height) / 2 - 2;
    ui->view.pixels = heap_caps_malloc((size_t)board->width * board->height * 2, MALLOC_CAP_SPIRAM);

    const settings_t *s = settings_get();
    ui->lat = s->weather_lat;
    ui->lon = s->weather_lon;
    bool place_known = ui->lat != 0.0f || ui->lon != 0.0f;
    if (!place_known)
    {
        ui->lat = 41.90f;   /* Rome until the place is set (Meteo or the companion) */
        ui->lon = 12.50f;
    }

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    ui->canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(ui->canvas, ui->view.pixels, board->width, board->height, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(ui->canvas);
    lv_obj_add_flag(ui->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->canvas, on_touch, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(ui->canvas, on_touch, LV_EVENT_PRESSING, ui);
    lv_obj_add_event_cb(ui->canvas, on_touch, LV_EVENT_RELEASED, ui);

    ui->when = text(screen, UI_FONT_MICRO, lv_color_make(150, 165, 190), LV_ALIGN_BOTTOM_MID, -44);
    ui->offset = text(screen, UI_FONT_HUD, lv_color_make(255, 190, 90), LV_ALIGN_BOTTOM_MID, -62);
    ui->name = text(screen, ui_font_body, lv_color_white(), LV_ALIGN_TOP_MID, 52);
    lv_obj_set_style_bg_color(ui->name, lv_color_make(10, 14, 30), 0);
    lv_obj_set_style_bg_opa(ui->name, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(ui->name, 12, 0);
    lv_obj_set_style_pad_ver(ui->name, 4, 0);
    lv_obj_set_style_radius(ui->name, 12, 0);
    lv_obj_add_flag(ui->name, LV_OBJ_FLAG_HIDDEN);

    if (!astri_sky_load())
    {
        lv_obj_t *error = text(screen, ui_font_body, lv_color_white(), LV_ALIGN_CENTER, 0);
        lv_label_set_text(error, "Catalogo del cielo mancante");
        return;
    }
    if (!place_known)
    {
        show_name(ui, "Posizione: Roma (imposta la città in Meteo)");
        ui->name_until = lv_tick_get() + 6000;
    }
    ui->frame = lv_timer_create(on_frame, FRAME_MS, ui);
    if (time(NULL) < CLOCK_VALID)
    {
        ui->waiting_clock = true;
        lv_obj_add_flag(ui->canvas, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui->name, "Sincronizzo l'ora...\nServe il Wi-Fi");
        lv_obj_set_style_text_align(ui->name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(ui->name, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(ui->name, LV_OBJ_FLAG_HIDDEN);
        ui->name_until = 0;
        return;
    }
    render(ui);
}

bool astri_ui_back(void)
{
    extapp_return_to_launcher();
    return true;
}
