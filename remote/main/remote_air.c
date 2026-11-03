/*
 * Air mouse: the watch's gyroscope moves the PC pointer.
 *
 * The pointer moves only while a finger rests on the pad (a "clutch"), so the
 * hand can be repositioned without moving it, unless "always on" is chosen.
 * Gravity from the accelerometer makes the mapping orientation independent:
 *   horizontal = rotation about the vertical (gravity) axis,
 *   vertical   = rotation about the device's horizontal "right" axis.
 * The gyroscope bias is learned whenever the watch is still.
 *
 * Only boards with an IMU (board_api imu_supported()). IMU calls belong to one
 * worker task, never to LVGL: the worker accumulates whole pixels in atomics,
 * an LVGL timer sends them as HID reports like the touchpad does.
 */
#include "remote.h"
#include "core/lv_port.h"
#include "core/sys.h"
#include "hardware/board.h"
#include "hardware/imu.h"
#include "ui/ui.h"
#include "ui/keyboard_layout.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#define AIR_INVERT_X 1
#define AIR_INVERT_Y 2
#define AIR_ALWAYS   4
#define CALIBRATION_SAMPLES 60

typedef struct
{
    lv_obj_t *pad;
    lv_obj_t *hint;
    lv_timer_t *timer;
    float dx, dy;
    float travel;
    uint32_t pressed_at;
    uint32_t version;
    bool clutch;
    bool scrolling;
    bool held;
    lv_point_t last;
    float sx, sy;
    bool calibrated_shown;
    lv_obj_t *scroll;
} air_t;

static air_t *s_air;
static atomic_bool s_wanted;      /* the air mouse page is open */
static atomic_bool s_worker;      /* the worker task exists */
static atomic_bool s_calibrated;  /* bias learned since the IMU started */
static atomic_bool s_recalibrate;
static atomic_int s_motion_x, s_motion_y;
static atomic_int s_imu_error;

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float dead_zone(float rate, float zone)
{
    return fabsf(rate) < zone ? 0 : rate - copysignf(zone, rate);
}

static void air_worker(void *arg)
{
    (void)arg;
    bool started = false;
    float bias[3] = {0}, gravity[3] = {0, 0, 1}, fx = 0, fy = 0;
    int samples = 0;
    int64_t last = 0;

    for (;;)
    {
        if (!atomic_load(&s_wanted))
        {
            if (started)
            {
                imu_stop();
                started = false;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (!started)
        {
            esp_err_t error = imu_start();
            atomic_store(&s_imu_error, error);
            if (error != ESP_OK)
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            started = true;
            samples = 0;
            fx = fy = 0;
            last = esp_timer_get_time();
            atomic_store(&s_calibrated, false);
        }
        if (atomic_exchange(&s_recalibrate, false))
        {
            samples = 0;
            atomic_store(&s_calibrated, false);
        }

        imu_sample_t sample;
        if (imu_read(&sample) != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int64_t now = esp_timer_get_time();
        float dt = (now - last) / 1e6f;
        last = now;
        if (dt > 0.05f)
            dt = 0.05f;

        for (int i = 0; i < 3; i++)
            gravity[i] = samples == 0 ? sample.acceleration_g[i] : gravity[i] * 0.92f + sample.acceleration_g[i] * 0.08f;

        float rate[3];
        for (int i = 0; i < 3; i++)
            rate[i] = sample.angular_dps[i] - bias[i];

        /* Learn the bias while still: fast average first, then a slow follow-up. */
        if (sqrtf(dot3(rate, rate)) < (samples < CALIBRATION_SAMPLES ? 6.0f : 3.0f))
        {
            float k = samples < CALIBRATION_SAMPLES ? 1.0f / (samples + 1) : 0.005f;
            for (int i = 0; i < 3; i++)
                bias[i] += (sample.angular_dps[i] - bias[i]) * k;
            samples++;
        }
        if (samples < CALIBRATION_SAMPLES)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        atomic_store(&s_calibrated, true);

        float length = sqrtf(dot3(gravity, gravity));
        if (length < 0.3f)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        float up[3] = {gravity[0] / length, gravity[1] / length, gravity[2] / length};
        /* Device X made horizontal; device Y if the watch is held on its side. */
        float right[3] = {1 - up[0] * up[0], -up[0] * up[1], -up[0] * up[2]};
        float right_length = sqrtf(dot3(right, right));
        if (right_length < 0.4f)
        {
            right[0] = -up[1] * up[0];
            right[1] = 1 - up[1] * up[1];
            right[2] = -up[1] * up[2];
            right_length = sqrtf(dot3(right, right));
        }
        for (int i = 0; i < 3; i++)
            right[i] /= right_length;

        float yaw = dead_zone(dot3(rate, up), 1.2f);
        float pitch = dead_zone(dot3(rate, right), 1.2f);
        /* Pixels per degree grow with speed: precise when slow, quick across the screen. */
        float base = 4.0f + 3.0f * remote_settings()->pointer_speed;
        uint8_t flags = remote_settings()->air_flags;
        fx += (flags & AIR_INVERT_X ? 1 : -1) * yaw * dt * base * (1 + fabsf(yaw) / 90);
        fy += (flags & AIR_INVERT_Y ? 1 : -1) * pitch * dt * base * (1 + fabsf(pitch) / 90);
        int px = (int)fx, py = (int)fy;
        fx -= px;
        fy -= py;
        if (px)
            atomic_fetch_add(&s_motion_x, px);
        if (py)
            atomic_fetch_add(&s_motion_y, py);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static int8_t bounded(float value)
{
    return (int8_t)(value < -127 ? -127 : value > 127 ? 127 : value);
}

static bool moving(const air_t *air)
{
    return air->clutch || (remote_settings()->air_flags & AIR_ALWAYS);
}

static void air_tick(lv_timer_t *timer)
{
    air_t *air = lv_timer_get_user_data(timer);
    int mx = atomic_exchange(&s_motion_x, 0);
    int my = atomic_exchange(&s_motion_y, 0);
    if (air->version != hid_link_version())
    {
        air->version = hid_link_version();
        air->dx = air->dy = 0;
        air->clutch = false;
        air->held = false;
        air->sx = air->sy = 0;
    }
    bool calibrated = atomic_load(&s_calibrated);
    if (calibrated != air->calibrated_shown)
    {
        air->calibrated_shown = calibrated;
        lv_label_set_text(air->hint, !calibrated ? "Tieni fermo\nl'orologio..."
                                     : remote_settings()->air_flags & AIR_ALWAYS ? "Muovi il polso\nPremi: trascina"
                                                                                  : "Tieni premuto\ne muovi il polso");
    }
    if (atomic_load(&s_imu_error) != ESP_OK)
    {
        lv_label_set_text(air->hint, "Sensore di\nmovimento assente");
        return;
    }
    if (air->scrolling && app_current() == &remote_air_app)
    {
        int8_t x = bounded(air->sx / 12), y = bounded(air->sy / 12);
        if ((x || y) && hid_link_pointer(0, 0, 0, y, -x))
        { air->sx -= x * 12; air->sy -= y * 12; }
        air->dx = air->dy = 0;
        return;
    }
    if (!moving(air) || app_current() != &remote_air_app)
    {
        air->dx = air->dy = 0; /* motion outside the clutch is discarded, never replayed */
        return;
    }
    air->dx = fmaxf(-512, fminf(512, air->dx + mx));
    air->dy = fmaxf(-512, fminf(512, air->dy + my));
    air->travel += abs(mx) + abs(my);
    int8_t dx = bounded(air->dx), dy = bounded(air->dy);
    if ((dx || dy) && hid_link_pointer(dx, dy, air->held ? 1 : 0, 0, 0))
    {
        air->dx -= dx;
        air->dy -= dy;
    }
}

static void air_touch(lv_event_t *e)
{
    air_t *air = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point = {0};
    if (indev) lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED)
    {
        air->last = point;
        if (!air->scrolling && (remote_settings()->air_flags & AIR_ALWAYS))
            air->held = remote_send_ok(hid_link_mouse(0, 0, 1, 0));
        air->clutch = true;
        air->travel = 0;
        air->pressed_at = lv_tick_get();
        atomic_store(&s_motion_x, 0); /* start from where the hand is now */
        atomic_store(&s_motion_y, 0);
        lv_obj_set_style_border_color(air->pad, ui_color(UI_COLOR_ORANGE), 0);
    }
    else if (code == LV_EVENT_PRESSING && air->clutch && air->scrolling)
    {
        air->sx = fmaxf(-512, fminf(512, air->sx + point.x - air->last.x));
        air->sy = fmaxf(-512, fminf(512, air->sy + point.y - air->last.y));
        air->last = point;
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        bool tap = code == LV_EVENT_RELEASED && air->travel < 12 && lv_tick_elaps(air->pressed_at) < 350;
        air->clutch = false;
        lv_obj_set_style_border_color(air->pad, ui_color(air->scrolling ? UI_COLOR_ORANGE : UI_COLOR_TEAL), 0);
        bool held = air->held;
        air->held = false;
        if (held) hid_link_mouse(0, 0, 0, 0);
        if (tap && !held && !air->scrolling)
            remote_send_ok(hid_link_click(1, 1));
    }
}

static void air_button(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    remote_send_ok(hid_link_click(i == 0 ? 1 : 2, 1));
}

static void option_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == 3)
    {
        atomic_store(&s_recalibrate, true);
        ui_toast("Appoggia l'orologio fermo per un secondo");
        return;
    }
    remote_settings_t settings = *remote_settings();
    settings.air_flags ^= i == 0 ? AIR_INVERT_X : i == 1 ? AIR_INVERT_Y : AIR_ALWAYS;
    remote_settings_save(&settings);
    ui_toast(settings.air_flags & (i == 0 ? AIR_INVERT_X : i == 1 ? AIR_INVERT_Y : AIR_ALWAYS) ? "Attivo" : "Disattivo");
    if (s_air != NULL)
        s_air->calibrated_shown = !atomic_load(&s_calibrated); /* refresh the hint */
}

static void air_options_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_t *page = ui_page(screen, "Air mouse");
    uint8_t flags = remote_settings()->air_flags;
    ui_row(page, LV_SYMBOL_SHUFFLE, UI_COLOR_TEAL, "Inverti orizzontale", flags & AIR_INVERT_X ? "Si" : "No",
           option_clicked, (void *)0);
    ui_row(page, LV_SYMBOL_SHUFFLE, UI_COLOR_TEAL, "Inverti verticale", flags & AIR_INVERT_Y ? "Si" : "No",
           option_clicked, (void *)1);
    ui_row(page, LV_SYMBOL_GPS, UI_COLOR_ORANGE, "Sempre attivo", flags & AIR_ALWAYS ? "Si" : "No", option_clicked,
           (void *)2);
    ui_row(page, LV_SYMBOL_REFRESH, UI_COLOR_BLUE, "Ricalibra", "Tieni fermo", option_clicked, (void *)3);
    ui_text(page, "Velocita: Opzioni > velocita del puntatore. Con \"Sempre attivo\" il puntatore segue sempre il polso.",
            true);
}

static const app_t s_air_options_app = {
    .id = "remote.air.options", .name = "Air mouse", .flags = APP_FLAG_HIDDEN, .create = air_options_create};

static void air_scroll(lv_event_t *e)
{
    (void)e;
    if (s_air == NULL)
        return;
    s_air->scrolling = !s_air->scrolling;
    s_air->sx = s_air->sy = s_air->dx = s_air->dy = 0;
    s_air->held = false;
    hid_link_release_all();
    lv_obj_set_style_bg_color(s_air->scroll, ui_color(s_air->scrolling ? UI_COLOR_ORANGE : UI_COLOR_CARD_HI), 0);
    lv_obj_set_style_border_color(s_air->pad, ui_color(s_air->scrolling ? UI_COLOR_ORANGE : UI_COLOR_TEAL), 0);
    if (s_air->scrolling)
        lv_label_set_text(s_air->hint, "Scorri con il dito\nin ogni direzione");
    else
        s_air->calibrated_shown = !atomic_load(&s_calibrated); /* the next tick restores the hint */
}

static void air_more(lv_event_t *e)
{
    (void)e;
    app_open_app(&s_air_options_app, NULL);
}

static void air_deleted(lv_event_t *e)
{
    air_t *air = lv_event_get_user_data(e);
    lv_timer_delete(air->timer);
    if (s_air == air)
    {
        s_air = NULL;
        atomic_store(&s_wanted, false);
    }
    free(air);
}

static void air_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    air_t *air = calloc(1, sizeof(*air));
    if (air == NULL)
    {
        ui_toast("Memoria insufficiente");
        return;
    }
    s_air = air;
    int32_t w = remote_screen_w(), h = remote_screen_h();
    bool round = board_info()->round, landscape = remote_landscape();

    /* Header: switcher on the left, title centred. */
    ui_key_rect_t head = remote_header_band();
    remote_switcher_button(screen);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Air mouse");
    lv_obj_set_style_text_font(title, UI_FONT_LARGE, 0);
    lv_obj_update_layout(title);
    lv_obj_set_pos(title, (w - lv_obj_get_width(title)) / 2, head.y + (head.h - lv_obj_get_height(title)) / 2);

    /* Pad and four controls: below the pad in portrait, beside it in landscape. */
    int32_t top = head.y + head.h + 8, bottom = round ? h * 91 / 100 : h - 8;
    int32_t row_h = 48, size, pad_x, pad_y;
    ui_key_rect_t controls[4];
    if (landscape)
    {
        int32_t split = w * 56 / 100;
        size = LV_MIN(split - 16, bottom - top);
        pad_x = (split - size) / 2;
        pad_y = top + (bottom - top - size) / 2;
        int32_t ch = (bottom - top) / 4;
        for (int i = 0; i < 4; i++)
            controls[i] = (ui_key_rect_t){split, top + ch * i, w - 8 - split, ch};
    }
    else
    {
        int32_t area = bottom - 2 * row_h - 8 - top;
        size = LV_MIN(w * 62 / 100, area);
        pad_x = (w - size) / 2;
        pad_y = top + (area - size) / 2;
        for (int row = 0; row < 2; row++)
        {
            ui_key_rect_t band = ui_keyboard_band(w, h, round, bottom - (2 - row) * row_h, row_h, 8);
            for (int col = 0; col < 2; col++)
                controls[row * 2 + col] = (ui_key_rect_t){band.x + band.w * col / 2, band.y,
                                                          band.w * (col + 1) / 2 - band.w * col / 2, band.h};
        }
    }

    air->pad = lv_obj_create(screen);
    lv_obj_remove_style_all(air->pad);
    lv_obj_set_pos(air->pad, pad_x, pad_y);
    lv_obj_set_size(air->pad, size, size);
    lv_obj_set_style_radius(air->pad, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(air->pad, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(air->pad, ui_color(UI_COLOR_CARD), 0);
    lv_obj_set_style_border_width(air->pad, 3, 0);
    lv_obj_set_style_border_color(air->pad, ui_color(UI_COLOR_TEAL), 0);
    lv_obj_set_scrollable(air->pad, false);
    lv_obj_set_press_lock(air->pad, true);
    lv_obj_set_gesture_bubble(air->pad, false);
    lv_obj_add_event_cb(air->pad, air_touch, LV_EVENT_PRESSED, air);
    lv_obj_add_event_cb(air->pad, air_touch, LV_EVENT_PRESSING, air);
    lv_obj_add_event_cb(air->pad, air_touch, LV_EVENT_RELEASED, air);
    lv_obj_add_event_cb(air->pad, air_touch, LV_EVENT_PRESS_LOST, air);
    air->hint = lv_label_create(air->pad);
    lv_label_set_text(air->hint, "Tieni fermo\nl'orologio...");
    lv_obj_set_width(air->hint, size * 80 / 100);
    lv_obj_set_style_text_font(air->hint, UI_FONT_BODY, 0);
    lv_obj_set_style_text_align(air->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(air->hint);

    static const char *const names[] = {"Sinistro", "Destro", "Scorri", "Altro"};
    for (int i = 0; i < 4; i++)
    {
        ui_key_rect_t r = controls[i];
        lv_obj_t *button = lv_obj_create(screen);
        lv_obj_remove_style_all(button);
        lv_obj_set_pos(button, r.x + 3, r.y + 3);
        lv_obj_set_size(button, r.w - 6, r.h - 6);
        lv_obj_set_scrollable(button, false);
        lv_obj_set_style_radius(button, 14, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(button, ui_color(UI_COLOR_CARD_HI), 0);
        lv_obj_set_style_bg_color(button, ui_accent(), LV_STATE_PRESSED);
        lv_obj_add_event_cb(button, i < 2 ? air_button : i == 2 ? air_scroll : air_more, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_port_set_focusable(button);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, names[i]);
        lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
        lv_obj_center(label);
        if (i == 2)
            air->scroll = button;
    }
    air->version = hid_link_version();
    air->calibrated_shown = false;
    atomic_store(&s_motion_x, 0);
    atomic_store(&s_motion_y, 0);
    atomic_store(&s_imu_error, ESP_OK);
    atomic_store(&s_wanted, true);
    if (!atomic_exchange(&s_worker, true) && !sys_task_create(air_worker, "air_mouse", 4096, NULL, 5, SYS_CORE_ANY))
        atomic_store(&s_worker, false);
    air->timer = lv_timer_create(air_tick, 20, air);
    lv_obj_add_event_cb(screen, air_deleted, LV_EVENT_DELETE, air);
}

static void air_pause(void)
{
    if (s_air != NULL) {
        s_air->clutch = false;
        s_air->held = false;
        s_air->sx = s_air->sy = s_air->dx = s_air->dy = 0;
    }
    atomic_store(&s_wanted, false);
    hid_link_release_all();
}

static void air_resume(void)
{
    atomic_store(&s_motion_x, 0);
    atomic_store(&s_motion_y, 0);
    atomic_store(&s_wanted, true);
}

const app_t remote_air_app = {.id = "remote.air",
                              .name = "Air mouse",
                              .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON | APP_FLAG_NO_BACK_GESTURE,
                              .create = air_create,
                              .pause = air_pause, .resume = air_resume};

void remote_air_register(void)
{
    if (!imu_supported())
        return;
    app_register(&remote_air_app);
    app_register(&s_air_options_app);
}
