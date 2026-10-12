/* ORBIT: an offline kinetic space for focus, breaks and gentle paced breathing.
 * Animation stops with the display; a one-shot system timer owns the deadline. */
#include "orbit.h"
#include "core/app.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/sys.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "services/sound.h"
#include "ui/ui.h"
#include "esp_timer.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct
{
    lv_obj_t *screen;
    lv_obj_t *content;
    lv_obj_t *planet;
    lv_obj_t *moon[4];
    lv_obj_t *headline;
    lv_obj_t *caption;
    lv_obj_t *border;
    lv_timer_t *animation;
    int page; /* 0 scenic, 1 controls, 2 session */
    int mode; /* 0 focus, 1 break, 2 breathing */
    int custom_minutes;
    int elapsed_angle;
    int last_seconds;
    bool paused;
    bool session;
    int total;
    int64_t remaining_us;
} orbit_ui_t;

static orbit_ui_t *s_ui;
static esp_timer_handle_t s_deadline_timer;
static portMUX_TYPE s_session_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_active;
static int64_t s_deadline;
static uint32_t s_session_packed;
static atomic_bool s_completed;
static atomic_uint s_finish_pending;
static atomic_uint s_minutes;
static atomic_uint s_sessions;
static const uint32_t s_colors[] = {0x9C7CFF, 0xFFB171, 0x5DE3CB};

static void render(orbit_ui_t *ui);

static void finish(void *arg)
{
    uint32_t packed = (uint32_t)(uintptr_t)arg;
    if ((packed & 3) == 0)
    {
        atomic_fetch_add(&s_minutes, packed >> 2);
        atomic_fetch_add(&s_sessions, 1);
        nvs_handle_t handle;
        if (nvs_open("orbit", NVS_READWRITE, &handle) == ESP_OK)
        {
            nvs_set_u32(handle, "minutes", atomic_load(&s_minutes));
            nvs_set_u32(handle, "sessions", atomic_load(&s_sessions));
            nvs_commit(handle);
            nvs_close(handle);
        }
    }
    if (!sound_alarm_playing()) sound_notification();
}

static void expired(void *arg)
{
    (void)arg;
    uint32_t packed = 0;
    portENTER_CRITICAL(&s_session_lock);
    if (s_active && esp_timer_get_time() >= s_deadline)
    {
        s_active = false;
        packed = s_session_packed;
    }
    portEXIT_CRITICAL(&s_session_lock);
    if (packed != 0)
    {
        atomic_store(&s_completed, true);
        atomic_store(&s_finish_pending, packed);
        power_wake(POWER_WAKE_NOTIFICATION);
    }
}

static int64_t remaining(orbit_ui_t *ui)
{
    if (ui->paused) return ui->remaining_us;
    int64_t deadline;
    portENTER_CRITICAL(&s_session_lock);
    deadline = s_deadline;
    portEXIT_CRITICAL(&s_session_lock);
    int64_t value = deadline - esp_timer_get_time();
    return value > 0 ? value : 0;
}

static void stop_session(orbit_ui_t *ui)
{
    portENTER_CRITICAL(&s_session_lock);
    s_active = false;
    portEXIT_CRITICAL(&s_session_lock);
    esp_timer_stop(s_deadline_timer);
    ui->session = false;
    ui->paused = false;
    atomic_store(&s_completed, false);
}

static bool arm(orbit_ui_t *ui, int64_t duration)
{
    if (duration < 1) return false;
    portENTER_CRITICAL(&s_session_lock);
    s_deadline = esp_timer_get_time() + duration;
    s_session_packed = ((uint32_t)(ui->total / 60) << 2) | ui->mode;
    s_active = true;
    portEXIT_CRITICAL(&s_session_lock);
    if (esp_timer_start_once(s_deadline_timer, duration) != ESP_OK)
    {
        portENTER_CRITICAL(&s_session_lock);
        s_active = false;
        portEXIT_CRITICAL(&s_session_lock);
        ui_toast("Timer non disponibile");
        return false;
    }
    return true;
}

static lv_obj_t *text(lv_obj_t *parent, const char *value, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, value);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    return object;
}

static void animate(lv_timer_t *timer)
{
    orbit_ui_t *ui = lv_timer_get_user_data(timer);
    uint32_t pending = atomic_exchange(&s_finish_pending, 0);
    if (pending != 0 && !sys_post(finish, (void *)(uintptr_t)pending))
        atomic_store(&s_finish_pending, pending);
    if (lv_screen_active() != ui->screen || power_screen() >= POWER_SCREEN_AOD) return;
    if (ui->page == 1) return;
    int seconds = ui->session ? (int)((remaining(ui) + 999999) / 1000000) : 0;
    if (ui->page == 2 && seconds != ui->last_seconds)
    {
        ui->last_seconds = seconds;
        if (seconds == 0)
        {
            lv_label_set_text(ui->headline, "Fatto.");
            lv_label_set_text(ui->caption, "Un piccolo passo. Un'orbita nuova.");
        }
        else if (ui->mode != 2)
        {
            lv_label_set_text_fmt(ui->headline, "%02d:%02d", seconds / 60, seconds % 60);
            lv_label_set_text(ui->caption, ui->paused ? "In pausa · tocca per riprendere" : "Tocca per mettere in pausa");
        }
        ui_border_progress_set(ui->border, ui->total - seconds, ui->total);
    }
    ui->elapsed_angle = (ui->elapsed_angle + (ui->paused ? 0 : 1)) % 360;
    int radius = board_info()->width * 39 / 100;
    for (int i = 0; i < 4; i++)
    {
        int angle = (ui->elapsed_angle + i * 90) % 360;
        lv_obj_align(ui->moon[i], LV_ALIGN_CENTER,
                     lv_trigo_cos(angle) * radius / 32768, lv_trigo_sin(angle) * radius / 32768 - 25);
    }
    if (ui->page == 2 && ui->mode == 2 && seconds > 0 && !ui->paused)
    {
        int elapsed = ui->total - seconds;
        int phase = elapsed % 10;
        int scale = phase < 5 ? 222 + phase * 7 : 257 - (phase - 5) * 7;
        lv_obj_set_style_transform_scale_x(ui->planet, scale, 0);
        lv_obj_set_style_transform_scale_y(ui->planet, scale, 0);
        lv_label_set_text(ui->headline, phase < 5 ? "Inspira" : "Espira");
        lv_label_set_text_fmt(ui->caption, "%d:%02d · senza trattenere", seconds / 60, seconds % 60);
    }
}

static void to_controls(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    ui->page = 1;
    render(ui);
}

static void planet_tap(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    if (ui->page == 0) to_controls(event);
    else if (remaining(ui) == 0)
    {
        stop_session(ui);
        ui->page = 1;
        render(ui);
    }
    else if (ui->paused)
    {
        if (arm(ui, ui->remaining_us))
        {
            ui->paused = false;
            ui->last_seconds = -1;
        }
    }
    else
    {
        ui->remaining_us = remaining(ui);
        portENTER_CRITICAL(&s_session_lock);
        s_active = false;
        portEXIT_CRITICAL(&s_session_lock);
        esp_timer_stop(s_deadline_timer);
        ui->paused = true;
        lv_label_set_text(ui->caption, "In pausa · tocca per riprendere");
    }
}

static void gesture(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (direction == LV_DIR_TOP) to_controls(event);
    else if (direction == LV_DIR_BOTTOM && ui->page != 0) orbit_back();
}

static void pressure(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    if (ui->page == 1 || ui->planet == NULL) return;
    lv_indev_t *indev = lv_indev_active();
    lv_point_t point;
    if (indev == NULL) return;
    lv_indev_get_point(indev, &point);
    lv_obj_align(ui->planet, LV_ALIGN_CENTER, (point.x - board_info()->width / 2) / 9,
                 -25 + (point.y - board_info()->height / 2) / 12);
}

static void release(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    if (ui->planet != NULL && ui->page != 1) lv_obj_align(ui->planet, LV_ALIGN_CENTER, 0, -25);
}

static void begin_mode(orbit_ui_t *ui, int mode)
{
    stop_session(ui);
    ui->mode = mode;
    ui->total = mode == 0 ? ui->custom_minutes * 60 : mode == 1 ? 5 * 60 : 2 * 60;
    ui->remaining_us = (int64_t)ui->total * 1000000;
    ui->session = arm(ui, ui->remaining_us);
    if (!ui->session) return;
    ui->page = 2;
    render(ui);
}

static void replace_session(bool confirmed, void *arg)
{
    if (confirmed) begin_mode(s_ui, (int)(intptr_t)arg);
}

static void start_mode(lv_event_t *event)
{
    int mode = (int)(intptr_t)lv_event_get_user_data(event);
    if (s_ui->session && remaining(s_ui) > 0)
        ui_confirm("Nuova orbita", "Terminare la sessione in corso?", "Inizia", s_colors[mode], replace_session, (void *)(intptr_t)mode);
    else begin_mode(s_ui, mode);
}

static void custom(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    ui->custom_minutes = lv_slider_get_value(lv_event_get_target_obj(event));
}

static void resume_session(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    ui->page = 2;
    render(ui);
}

static void stop(lv_event_t *event)
{
    orbit_ui_t *ui = lv_event_get_user_data(event);
    stop_session(ui);
    ui->page = 0;
    render(ui);
}

static void leave(lv_event_t *event)
{
    (void)event;
    extapp_return_to_launcher();
}

static void scene(orbit_ui_t *ui)
{
    uint32_t color = ui->page == 0 ? s_colors[0] : s_colors[ui->mode];
    lv_obj_t *root = ui->content;
    lv_obj_set_style_bg_color(root, lv_color_hex(0x070A15), 0);
    lv_obj_set_style_bg_grad_color(root, lv_color_hex(0x18132C), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_clickable(root, true);
    lv_obj_add_event_cb(root, planet_tap, LV_EVENT_CLICKED, ui);
    lv_obj_set_gesture_bubble(root, false);
    lv_obj_add_event_cb(root, gesture, LV_EVENT_GESTURE, ui);
    lv_obj_add_event_cb(root, pressure, LV_EVENT_PRESSING, ui);
    lv_obj_add_event_cb(root, release, LV_EVENT_RELEASED, ui);
    for (int i = 0; i < 22; i++)
    {
        lv_obj_t *star = lv_obj_create(root);
        lv_obj_remove_style_all(star);
        lv_obj_set_size(star, i % 5 == 0 ? 3 : 1, i % 5 == 0 ? 3 : 1);
        lv_obj_set_style_bg_color(star, lv_color_hex(0xBCC9DF), 0);
        lv_obj_set_style_bg_opa(star, i % 5 == 0 ? LV_OPA_70 : LV_OPA_30, 0);
        lv_obj_set_pos(star, (i * 83 + 19) % board_info()->width, (i * 137 + 43) % board_info()->height);
        lv_obj_set_clickable(star, false);
    }
    int diameter = board_info()->width * 72 / 100;
    ui->planet = lv_obj_create(root);
    lv_obj_remove_style_all(ui->planet);
    lv_obj_set_size(ui->planet, diameter, diameter);
    lv_obj_set_style_radius(ui->planet, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ui->planet, lv_color_hex(color), 0);
    lv_obj_set_style_bg_grad_color(ui->planet, lv_color_hex(0x111528), 0);
    lv_obj_set_style_bg_grad_dir(ui->planet, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(ui->planet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui->planet, 2, 0);
    lv_obj_set_style_border_color(ui->planet, lv_color_lighten(lv_color_hex(color), 100), 0);
    lv_obj_set_style_border_opa(ui->planet, LV_OPA_70, 0);
    lv_obj_set_style_shadow_color(ui->planet, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(ui->planet, 28, 0);
    lv_obj_set_style_shadow_opa(ui->planet, LV_OPA_20, 0);
    lv_obj_set_clickable(ui->planet, false);
    lv_obj_set_scrollable(ui->planet, false);
    lv_obj_align(ui->planet, LV_ALIGN_CENTER, 0, -25);
    lv_obj_t *shine = lv_obj_create(ui->planet);
    lv_obj_remove_style_all(shine);
    lv_obj_set_size(shine, diameter - 20, diameter - 20);
    lv_obj_set_style_radius(shine, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(shine, 1, 0);
    lv_obj_set_style_border_color(shine, lv_color_hex(0xE5DDFF), 0);
    lv_obj_set_style_border_opa(shine, LV_OPA_30, 0);
    lv_obj_center(shine);
    lv_obj_set_clickable(shine, false);
    lv_obj_set_scrollable(shine, false);
    for (int i = 0; i < 4; i++)
    {
        ui->moon[i] = lv_obj_create(root);
        lv_obj_remove_style_all(ui->moon[i]);
        lv_obj_set_size(ui->moon[i], 5 + i * 2, 5 + i * 2);
        lv_obj_set_style_radius(ui->moon[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ui->moon[i], lv_color_hex(i == 1 ? 0x69E6DD : 0xDDD4FF), 0);
        lv_obj_set_style_bg_opa(ui->moon[i], LV_OPA_COVER, 0);
        lv_obj_set_clickable(ui->moon[i], false);
    }
    ui->headline = text(root, ui->page == 0 ? "ORBIT" : "",
                        ui->page == 0 ? ui_font_title : ui->mode == 2 ? ui_font_headline : ui_font_big, 0xFFFFFF);
    lv_obj_align(ui->headline, LV_ALIGN_CENTER, 0, -25);
    ui->caption = text(root, ui->page == 0 ? "Il tuo tempo. Il tuo spazio." : "", UI_FONT_SMALL, 0xD2CCDF);
    lv_obj_align(ui->caption, LV_ALIGN_CENTER, 0, diameter / 2 - 34);
    if (ui->page == 0)
    {
        lv_obj_t *hint = text(root, "Tocca · esplora · respira", UI_FONT_SMALL, 0x9696B4);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, diameter / 2 - 6);
    }
    else
    {
        lv_obj_t *name = text(root, ui->mode == 0 ? "FOCUS" : ui->mode == 1 ? "PAUSA" : "RESPIRA", UI_FONT_SMALL, 0xDBD5F0);
        lv_obj_align(name, LV_ALIGN_CENTER, 0, -board_info()->width / 3);
        lv_obj_t *hint = text(root, "Scorri in alto per i controlli", UI_FONT_SMALL, 0x9696B4);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, diameter / 2 - 6);
    }
    ui->last_seconds = -1;
    ui_border_progress_color(ui->border, color);
    if (ui->border != NULL) lv_obj_set_hidden(ui->border, ui->page != 2);
    animate(ui->animation);
}

static void controls(orbit_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Il tuo spazio");
    ui_text(page, "Una cosa alla volta.", true);
    if (ui->session)
    {
        ui_button(page, "Torna alla sessione", 0x4B4568, resume_session, ui);
        ui_button(page, "Termina sessione", 0x493339, stop, ui);
    }
    lv_obj_t *slider = ui_slider_row(page, LV_SYMBOL_LOOP, "Minuti di concentrazione", 1, 90, ui->custom_minutes, custom, ui);
    (void)slider;
    ui_button(page, "Concentrazione", s_colors[0], start_mode, (void *)0);
    ui_button(page, "Pausa · 5 minuti", s_colors[1], start_mode, (void *)1);
    ui_button(page, "Respira · 2 minuti", s_colors[2], start_mode, (void *)2);
    char stats[96];
    snprintf(stats, sizeof(stats), "%lu sessioni · %lu minuti dedicati a ciò che conta.",
             (unsigned long)atomic_load(&s_sessions), (unsigned long)atomic_load(&s_minutes));
    ui_section(page, "Il tuo percorso");
    ui_text(page, stats, true);
    ui_text(page, "Funziona offline. Il display può riposare: il timer continua.", true);
    ui_button(page, "Torna al launcher", 0x28283C, leave, ui);
}

static void render(orbit_ui_t *ui)
{
    /* Delete a scene container instead of repeatedly accumulating callbacks. */
    if (ui->content != NULL) lv_obj_delete(ui->content);
    ui->content = lv_obj_create(ui->screen);
    lv_obj_remove_style_all(ui->content);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(ui->content, false);
    lv_obj_move_to_index(ui->content, 0);
    ui->planet = NULL;
    if (ui->page == 1)
    {
        if (ui->border != NULL) lv_obj_set_hidden(ui->border, true);
        controls(ui);
    }
    else scene(ui);
}

bool orbit_back(void)
{
    if (s_ui == NULL || s_ui->page == 0)
    {
        extapp_return_to_launcher();
        return true;
    }
    s_ui->page = s_ui->page == 2 ? 1 : s_ui->session ? 2 : 0;
    render(s_ui);
    return true;
}

void orbit_create(lv_obj_t *screen)
{
    s_ui = calloc(1, sizeof(*s_ui));
    ESP_ERROR_CHECK(s_ui == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    s_ui->screen = screen;
    s_ui->custom_minutes = 25;
    nvs_handle_t handle;
    if (nvs_open("orbit", NVS_READONLY, &handle) == ESP_OK)
    {
        uint32_t value = 0;
        if (nvs_get_u32(handle, "minutes", &value) == ESP_OK) atomic_store(&s_minutes, value);
        if (nvs_get_u32(handle, "sessions", &value) == ESP_OK) atomic_store(&s_sessions, value);
        nvs_close(handle);
    }
    const esp_timer_create_args_t args = {.callback = expired, .name = "orbit_deadline"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_deadline_timer));
    s_ui->border = ui_border_progress_create(screen, s_colors[0]);
    s_ui->animation = lv_timer_create(animate, 60, s_ui);
    ESP_ERROR_CHECK(s_ui->animation == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    render(s_ui);
}
