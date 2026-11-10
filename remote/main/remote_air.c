/*
 * Air mouse: the watch's gyroscope moves the PC pointer.
 *
 * The whole screen is the pad, with the touch mouse's gestures adapted:
 *   finger resting + wrist moving   pointer (a "clutch": the hand can be repositioned)
 *   tap                             left click
 *   finger and wrist still 0.6 s    right click
 *   along an edge                   scroll, volume, brightness (remote_gestures.c)
 * With "always on" the wrist always moves the pointer, and a resting finger
 * plus a wrist movement drags (left button held until the finger lifts).
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
#include "core/settings.h"
#include "core/sys.h"
#include "hardware/board.h"
#include "hardware/imu.h"
#include "ui/ui.h"
#include "ui/keyboard_layout.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#define AIR_INVERT_X 1
#define AIR_INVERT_Y 2
#define AIR_ALWAYS   4
#define CALIBRATION_SAMPLES 60

/* What the finger on the pad is doing (one-touch panels). */
typedef enum
{
    AIR_IDLE,
    AIR_UNDECIDED, /* down, nothing moved yet: tap, hold or pointer */
    AIR_POINTER,   /* the wrist moves the pointer (clutch, or drag when always on) */
    AIR_EDGE,      /* finger along an edge: scroll, volume, brightness */
    AIR_DONE,      /* right click sent: the rest of this touch is ignored */
} air_touch_t;

#define AIR_SLOP 8      /* finger px before a touch counts as a move */
#define AIR_STILL 20    /* wrist px before the hand counts as moving */
#define AIR_HOLD_MS 600 /* finger and wrist still this long: right click */
#define AIR_TAP_MS 400
#define AIR_SETTLE_MS 150   /* wrist motion ignored after a press or release: the finger jolts the watch */


typedef struct
{
    lv_obj_t *pad, *ring, *hint, *pill, *marks[4];
    lv_timer_t *timer;
    lv_point_t start, last;
    float dx, dy; /* pointer counts not sent yet */
    float travel; /* wrist travel during this touch */
    float along;  /* edge travel not turned into steps yet */
    uint32_t pressed_at, released_at;
    uint32_t version;
    remote_edge_t edge;
    air_touch_t state;
    bool held; /* left button down: drag with "always on" */
    bool calibrated_shown;
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

/* Gyro bias learned once and kept ("remote" namespace): later starts are ready at once. */
static bool bias_load(float bias[3])
{
    nvs_handle_t nvs;
    size_t size = 3 * sizeof(float);
    if (nvs_open("remote", NVS_READONLY, &nvs) != ESP_OK)
        return false;
    bool ok = nvs_get_blob(nvs, "air_bias", bias, &size) == ESP_OK && size == 3 * sizeof(float);
    nvs_close(nvs);
    for (int i = 0; ok && i < 3; i++)
        ok = isfinite(bias[i]) && fabsf(bias[i]) < 30.0f;
    return ok;
}

static void bias_save(const float bias[3])
{
    nvs_handle_t nvs;
    if (nvs_open("remote", NVS_READWRITE, &nvs) != ESP_OK)
        return;
    nvs_set_blob(nvs, "air_bias", bias, 3 * sizeof(float));
    nvs_commit(nvs);
    nvs_close(nvs);
}

static float dead_zone(float rate, float zone)
{
    return fabsf(rate) < zone ? 0 : rate - copysignf(zone, rate);
}

/*
 * The device's "right" side for the chosen grip, in IMU axes (parallel to the
 * native panel: X along its first, Y along its second side). Auto follows the
 * screen orientation on rotated boards (a bar held like its screen) and keeps
 * the gravity-only mapping on round or unrotated watches. False = automatic.
 */
static bool grip_axis(float axis[3])
{
    static const int8_t sides[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}}; /* quarter turns 0..3 */
    int grip = remote_settings()->air_grip;
    int turns;
    if (grip == 0)
    {
        turns = settings_get()->screen_rotation & 3; /* follows the display mounting */
        if (turns == 0)
            return false;
    }
    else
    {
        /* Horizontal = the landscape mounting (one quarter turn), vertical = portrait. */
        static const int8_t grip_turns[5] = {0, 1, 0, 3, 2};
        turns = grip_turns[grip > 4 ? 0 : grip];
    }
    axis[0] = sides[turns][0];
    axis[1] = sides[turns][1];
    axis[2] = 0;
    return true;
}

static void air_worker(void *arg)
{
    (void)arg;
    bool started = false;
    float bias[3] = {0}, gravity[3] = {0, 0, 1}, fx = 0, fy = 0;
    int samples = 0;
    int64_t last = 0;
    bool first_sample = true;

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
            first_sample = true;
            fx = fy = 0;
            last = esp_timer_get_time();
            samples = bias_load(bias) ? CALIBRATION_SAMPLES : 0;
            atomic_store(&s_calibrated, samples == CALIBRATION_SAMPLES);
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
        if (first_sample)
        {
            ESP_LOGI("air_mouse", "IMU sample: gyro %.2f %.2f %.2f dps", (double)sample.angular_dps[0],
                     (double)sample.angular_dps[1], (double)sample.angular_dps[2]);
            first_sample = false;
        }
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
            if (++samples == CALIBRATION_SAMPLES)
            {
                bias_save(bias); /* first calibration, or one asked from the options */
                ESP_LOGI("air_mouse", "Calibration ready");
            }
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
        float right[3];
        float right_length = 0;
        float axis[3];
        if (grip_axis(axis))
        {
            /* The chosen grip's "right" side, made horizontal. */
            float along = dot3(axis, up);
            for (int i = 0; i < 3; i++)
                right[i] = axis[i] - along * up[i];
            right_length = sqrtf(dot3(right, right));
        }
        else
        {
            /* Device X made horizontal; device Y if the watch is held on its side. */
            right[0] = 1 - up[0] * up[0];
            right[1] = -up[0] * up[1];
            right[2] = -up[0] * up[2];
            right_length = sqrtf(dot3(right, right));
        }
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

static bool always_on(void)
{
    return (remote_settings()->air_flags & AIR_ALWAYS) != 0;
}

static void ring_color(air_t *air, uint32_t color)
{
    lv_obj_set_style_border_color(air->ring, ui_color(color), 0);
}

static void pill_show(air_t *air, const char *text)
{
    lv_label_set_text(lv_obj_get_child(air->pill, 0), text);
    lv_obj_remove_flag(air->pill, LV_OBJ_FLAG_HIDDEN);
}

/* Finger lifted, link lost or page left: nothing stays pressed or queued. */
static void touch_end(air_t *air)
{
    if (air->held)
        hid_link_mouse(0, 0, 0, 0);
    air->held = false;
    air->state = AIR_IDLE;
    air->dx = air->dy = 0;
    air->released_at = lv_tick_get();
    lv_obj_add_flag(air->pill, LV_OBJ_FLAG_HIDDEN);
    remote_edge_light(air->marks, REMOTE_EDGE_NONE);
    ring_color(air, UI_COLOR_TEAL);
}

static const char *idle_hint(void)
{
    return !atomic_load(&s_calibrated) ? "Tieni fermo\nl'orologio..."
           : always_on()               ? "Muovi il polso\nTocca e muovi: trascina"
                                       : "Tieni il dito\ne muovi il polso";
}

static void air_tick(lv_timer_t *timer)
{
    air_t *air = lv_timer_get_user_data(timer);
    int mx = atomic_exchange(&s_motion_x, 0);
    int my = atomic_exchange(&s_motion_y, 0);
    if (air->version != hid_link_version() || app_current() != &remote_air_app)
    {
        air->version = hid_link_version();
        if (air->state != AIR_IDLE)
            touch_end(air);
        air->dx = air->dy = 0; /* motion off the page is discarded, never replayed */
        return;
    }
    bool calibrated = atomic_load(&s_calibrated);
    if (calibrated != air->calibrated_shown)
    {
        air->calibrated_shown = calibrated;
        lv_label_set_text(air->hint, idle_hint());
    }
    if (atomic_load(&s_imu_error) != ESP_OK)
    {
        lv_label_set_text(air->hint, "Sensore di\nmovimento assente");
        return;
    }

    /* The jolt of a release would move the pointer between the clicks of a double click. */
    if (air->state == AIR_IDLE && lv_tick_elaps(air->released_at) < AIR_SETTLE_MS)
        return;
    if (air->state == AIR_UNDECIDED)
    {
        uint32_t elapsed = lv_tick_elaps(air->pressed_at);
        if (elapsed < AIR_SETTLE_MS)
            return; /* the jolt of the press is no hand movement */
        /* Held back while undecided: a tap clicks where the pointer was. */
        air->travel += abs(mx) + abs(my);
        air->dx += mx;
        air->dy += my;
        mx = my = 0;
        if (air->travel >= AIR_STILL && air->edge == REMOTE_EDGE_NONE)
        {
            /* The hand moved: pointer (clutch) or drag (always on). */
            air->state = AIR_POINTER;
            ring_color(air, UI_COLOR_ORANGE);
            if (always_on() && remote_send_ok(hid_link_mouse(0, 0, 1, 0)))
            {
                air->held = true;
                pill_show(air, "Trascina");
            }
        }
        else if (elapsed >= AIR_HOLD_MS && air->travel < AIR_STILL)
        {
            air->state = AIR_DONE;
            air->dx = air->dy = 0;
            if (remote_send_ok(hid_link_click(2, 1)))
                pill_show(air, "Clic destro");
            return;
        }
        else
            return;
    }
    /* The wrist drives the pointer while the finger holds it, or always when chosen. */
    bool pointer = air->state == AIR_POINTER || (air->state == AIR_IDLE && always_on());
    if (!pointer)
    {
        air->dx = air->dy = 0;
        return;
    }
    air->dx = fmaxf(-512, fminf(512, air->dx + mx));
    air->dy = fmaxf(-512, fminf(512, air->dy + my));
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
    if (indev == NULL)
        return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED)
    {
        air->start = air->last = point;
        air->pressed_at = lv_tick_get();
        air->travel = 0;
        air->along = 0;
        air->edge = remote_edge_at(point);
        air->state = AIR_UNDECIDED;
        if (!always_on())
        {
            atomic_store(&s_motion_x, 0); /* the clutch starts from where the hand is now */
            atomic_store(&s_motion_y, 0);
        }
        return;
    }
    if (code == LV_EVENT_PRESSING)
    {
        int32_t dx = point.x - air->last.x, dy = point.y - air->last.y;
        air->last = point;
        if (air->state == AIR_UNDECIDED)
        {
            int32_t tx = point.x - air->start.x, ty = point.y - air->start.y;
            if (LV_ABS(tx) + LV_ABS(ty) < AIR_SLOP)
                return;
            if (remote_edge_claims(air->edge, tx, ty))
            {
                air->state = AIR_EDGE;
                air->dx = air->dy = 0; /* wrist motion held back while undecided is dropped */
                remote_edge_light(air->marks, air->edge);
                pill_show(air, remote_edge_name(air->edge));
                dx = tx; /* the slop travel counts too */
                dy = ty;
            }
            else
            {
                air->state = AIR_POINTER; /* a finger slide is no gesture here: keep the clutch */
                ring_color(air, UI_COLOR_ORANGE);
            }
        }
        if (air->state == AIR_EDGE)
            remote_edge_move(air->edge, remote_edge_vertical(air->edge) ? dy : dx, &air->along);
        return;
    }
    /* Released or lost */
    bool tap =
        code == LV_EVENT_RELEASED && air->state == AIR_UNDECIDED && lv_tick_elaps(air->pressed_at) < AIR_TAP_MS;
    if (tap)
        remote_send_ok(hid_link_click(1, 1));
    touch_end(air);
}

static const char *grip_name(uint8_t grip)
{
    static const char *const names[] = {"Auto", "Orizzontale", "Verticale", "Orizz. capovolta", "Vert. capovolta"};
    return names[grip < 5 ? grip : 0];
}

static void option_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == 4)
    {
        /* Cycle the grip; the worker reads it on its next sample. */
        remote_settings_t settings = *remote_settings();
        settings.air_grip = (uint8_t)((settings.air_grip + 1) % 5);
        remote_settings_save(&settings);
        lv_obj_t *value = ui_row_value(lv_event_get_current_target_obj(e));
        if (value != NULL)
            lv_label_set_text(value, grip_name(settings.air_grip));
        return;
    }
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
        lv_label_set_text(s_air->hint, idle_hint());
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
    ui_row(page, LV_SYMBOL_LOOP, UI_COLOR_PURPLE, "Impugnatura", grip_name(remote_settings()->air_grip),
           option_clicked, (void *)4);
    ui_row(page, LV_SYMBOL_REFRESH, UI_COLOR_BLUE, "Ricalibra", "Tieni fermo", option_clicked, (void *)3);
    ui_text(page,
            "Sul pad: tocca per il clic, tieni fermi dito e polso per il destro. Bordi: scorrimento, volume e "
            "luminosità come nel mouse. Con \"Sempre attivo\" il puntatore segue sempre il polso e dito + polso "
            "trascina. Velocità: Opzioni > velocità del puntatore.",
            true);
}

static const app_t s_air_options_app = {
    .id = "remote.air.options", .name = "Air mouse", .flags = APP_FLAG_HIDDEN, .create = air_options_create};

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
    lv_obj_set_scrollable(screen, false);
    int32_t w = remote_screen_w(), h = remote_screen_h();
    bool round = board_info()->round;

    /* The whole screen is the pad, as in the touch mouse */
    air->pad = lv_obj_create(screen);
    lv_obj_remove_style_all(air->pad);
    lv_obj_set_size(air->pad, w, h);
    lv_obj_set_style_radius(air->pad, round ? LV_RADIUS_CIRCLE : 0, 0);
    lv_obj_set_style_bg_opa(air->pad, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(air->pad, ui_color(UI_COLOR_CARD), 0);
    lv_obj_set_scrollable(air->pad, false);
    lv_obj_set_press_lock(air->pad, true);
    lv_obj_set_gesture_bubble(air->pad, false);
    lv_obj_add_event_cb(air->pad, air_touch, LV_EVENT_PRESSED, air);
    lv_obj_add_event_cb(air->pad, air_touch, LV_EVENT_PRESSING, air);
    lv_obj_add_event_cb(air->pad, air_touch, LV_EVENT_RELEASED, air);
    lv_obj_add_event_cb(air->pad, air_touch, LV_EVENT_PRESS_LOST, air);
    bool rails = remote_rails_wanted();
    if (rails)
    {
        /* Bar panels: rails and click keys; the pad keeps tap, hold and drag */
        ui_key_rect_t area = remote_rails_create(screen, UI_COLOR_ORANGE);
        lv_obj_set_pos(air->pad, area.x, area.y);
        lv_obj_set_size(air->pad, area.w, area.h);
        lv_obj_set_style_radius(air->pad, 22, 0);
        lv_obj_set_style_bg_grad_color(air->pad, lv_color_mix(ui_color(UI_COLOR_ORANGE), lv_color_hex(0x07080D), 30), 0);
        lv_obj_set_style_bg_grad_dir(air->pad, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(air->pad, 1, 0);
        lv_obj_set_style_border_color(air->pad, ui_color(UI_COLOR_ORANGE), 0);
        lv_obj_set_style_border_opa(air->pad, LV_OPA_30, 0);
        w = area.w;
        h = area.h;
    }
    else
    {
        for (int i = 0; i < 4; i++)
            air->marks[i] = remote_edge_mark(air->pad, (remote_edge_t)i);
    }

    /* The ring tells what the wrist does: teal idle, orange while it moves the pointer */
    int32_t size = LV_MIN(w, h) * 46 / 100;
    air->ring = lv_obj_create(air->pad);
    lv_obj_remove_style_all(air->ring);
    lv_obj_set_size(air->ring, size, size);
    bool bar = !rails && h <= 200 && w >= 2 * h;
    bool narrow = !rails && w <= 200 && h >= 2 * w;
    bool wide = rails && w > 2 * h;   /* landscape bar: a capsule along the pad */
    if (wide)
        lv_obj_set_size(air->ring, w - 16, h - 12);
    else if (rails)
        lv_obj_set_size(air->ring, LV_MIN(w - 16, h - 16), LV_MIN(w - 16, h - 16));
    else if (bar)
        lv_obj_set_size(air->ring, w / 2, h / 2);
    else if (narrow)
        lv_obj_set_size(air->ring, w - 24, 180);
    lv_obj_set_style_radius(air->ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(air->ring, 3, 0);
    lv_obj_set_style_border_color(air->ring, ui_color(UI_COLOR_TEAL), 0);
    lv_obj_remove_flag(air->ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(air->ring);
    air->hint = lv_label_create(air->ring);
    lv_label_set_text(air->hint, "Tieni fermo\nl'orologio...");
    lv_obj_set_width(air->hint, size * 84 / 100);
    if (wide)
        lv_obj_set_width(air->hint, w - 40);
    else if (rails)
        lv_obj_set_width(air->hint, LV_MIN(w - 16, h - 16) - 20);
    else if (bar)
        lv_obj_set_width(air->hint, w / 2 - 20);
    else if (narrow)
        lv_obj_set_width(air->hint, w - 48);
    lv_obj_set_style_text_font(air->hint, rails ? UI_FONT_CAPTION : UI_FONT_SMALL, 0);
    lv_obj_set_style_text_align(air->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(air->hint);

    air->pill = lv_obj_create(air->pad);
    lv_obj_remove_style_all(air->pill);
    lv_obj_set_size(air->pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(air->pill, 18, 0);
    lv_obj_set_style_pad_ver(air->pill, 8, 0);
    lv_obj_set_style_radius(air->pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(air->pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(air->pill, ui_accent(), 0);
    lv_obj_remove_flag(air->pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *pill_text = lv_label_create(air->pill);
    lv_label_set_text(pill_text, "");
    lv_obj_set_style_text_font(pill_text, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(pill_text, lv_color_black(), 0);
    lv_obj_center(air->pill);
    lv_obj_add_flag(air->pill, LV_OBJ_FLAG_HIDDEN);

    /* Header over the pad: switcher on the left, options on the right */
    ui_key_rect_t head = remote_header_band();
    remote_switcher_button(screen);
    lv_obj_t *more = lv_obj_create(screen);
    lv_obj_remove_style_all(more);
    lv_obj_set_pos(more, head.x + head.w - 47, head.y + 3);
    lv_obj_set_size(more, 44, head.h - 6);
    lv_obj_set_scrollable(more, false);
    lv_obj_set_style_radius(more, 14, 0);
    lv_obj_set_style_bg_opa(more, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(more, ui_color(UI_COLOR_CARD_HI), 0);
    lv_obj_set_style_bg_color(more, ui_accent(), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(more, 8);
    lv_obj_add_event_cb(more, air_more, LV_EVENT_CLICKED, NULL);
    lv_port_set_focusable(more);
    lv_obj_t *more_label = lv_label_create(more);
    lv_label_set_text(more_label, LV_SYMBOL_BARS);
    lv_obj_set_style_text_color(more_label, ui_accent(), 0);
    lv_obj_center(more_label);

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
    if (s_air != NULL)
        touch_end(s_air);
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
