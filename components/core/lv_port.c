#include "core/lv_port.h"

#include "core/power.h"
#include "core/settings.h"
#include "core/state.h"

#include "hardware/board.h"
#include "hardware/display.h"
#include "hardware/knob.h"
#include "hardware/touch.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "lv_port";

#define TASK_STACK        (12 * 1024)
#define TASK_PRIORITY     4
#define TASK_CORE         1
#define MAX_SLEEP_MS      1000
#define ASYNC_QUEUE_LEN   16
#define BOOST_HOLD_MS     60      /* keep 240 MHz between frames of an animation */

/* Edge swipe = "back", like on most smartwatches. */
#define EDGE_ZONE_PX      28
#define EDGE_SWIPE_DX     70
#define EDGE_SWIPE_MAX_DY 60

/* Knob: pixels scrolled per detent, and how deep to look for a scrollable object. */
#define KNOB_SCROLL_PX    56
#define KNOB_SEARCH_DEPTH 4

/* Focus navigation: most focusable objects on one screen, and when to look at a new screen. */
#define FOCUS_MAX         64
#define FOCUS_SETTLE_MS   300     /* after the 220 ms slide: coordinates are final */

typedef struct
{
    ui_async_fn_t fn;
    void *arg;
} async_item_t;

static lv_display_t *s_disp = NULL;
static lv_indev_t *s_indev = NULL;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_flush_done = NULL;
/* LVGL uses notification slot 0 for its draw worker. Never share it for UI wakeups. */
static SemaphoreHandle_t s_wake = NULL;
static SemaphoreHandle_t s_refresh_done = NULL;
static atomic_bool s_refresh_request = false;
static QueueHandle_t s_async = NULL;

static volatile bool s_running = true;
static bool s_headless;

/* Health: when the current lvgl pass started (0 = waiting), slowest pass, lost flush interrupts. */
static _Atomic int64_t s_pass_start_us = 0;
static _Atomic uint32_t s_max_pass_ms = 0;
static _Atomic uint32_t s_flush_timeouts = 0;

/* Synthetic touch (monkey test): 0 = none, else FAKE_xxx flags | x << 12 | y. */
#define FAKE_PRESSED  (1u << 30)
#define FAKE_RELEASED (1u << 29)
static _Atomic uint32_t s_fake_touch = 0;
static volatile bool s_touch_irq = false;
static void (*s_back_cb)(void) = NULL;
static bool (*s_back_allowed)(void) = NULL;

/* Touch tracking (only used in the lvgl task). */
static int s_released_reads = 0;
static bool s_swallow_until_release = false;
static bool s_edge_candidate = false;
static bool s_back_gesture = false;
static int16_t s_start_x = 0;
static int16_t s_start_y = 0;
static bool s_was_pressed = false;

/* Knob detents not handled yet (written by the knob driver, read by the lvgl task). */
static _Atomic int s_knob_delta = 0;
static uint32_t s_knob_event = 0;

/* Focus navigation (lvgl task only). */
static lv_indev_t *s_encoder = NULL;
static lv_group_t *s_group = NULL;
static bool s_focus_mode = false;
static int s_enc_diff = 0;
static bool s_enc_pressed = false;
static lv_color_t s_focus_color;
static lv_obj_t *s_focus_root = NULL;      /* screen / layer the group was built from */
static uint32_t s_focus_layer_count = 0;   /* top layer children (dialogs) at that time */
static lv_timer_t *s_focus_timer = NULL;
static bool s_focus_quiet = false;         /* focus set by a rebuild, not by the user: no scrolling */

/* ------------------------------------------------------------- display */

static void flush_done_isr(void *ctx)
{
    (void)ctx;

    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    portYIELD_FROM_ISR(woken);
}

/* Only the UI task touches these; no framebuffer is retained between requests. */
static lv_draw_buf_t *s_capture;
static uint32_t s_capture_pixels;

static void capture_flush(const lv_area_t *area, const uint8_t *pixels)
{
    lv_draw_buf_t *capture = s_capture;
    if (capture == NULL) return;
    int width = lv_area_get_width(area);
    int stride = (int)lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565_SWAPPED);
    for (int y = area->y1; y <= area->y2; y++) {
        if (y < 0 || y >= (int)capture->header.h) continue;
        uint16_t *dst = (uint16_t *)(capture->data + (size_t)y * capture->header.stride);
        const uint8_t *src = pixels + (size_t)(y - area->y1) * stride;
        for (int x = area->x1; x <= area->x2; x++) {
            if (x < 0 || x >= (int)capture->header.w) continue;
            int offset = (x - area->x1) * 2;
            dst[x] = (uint16_t)((uint16_t)src[offset] << 8 | src[offset + 1]);
            s_capture_pixels++;
        }
    }
}

lv_draw_buf_t *lv_port_capture_frame(void)
{
    if (s_disp == NULL || s_capture != NULL) return NULL;
    int w = lv_display_get_horizontal_resolution(s_disp);
    int h = lv_display_get_vertical_resolution(s_disp);
    lv_draw_buf_t *frame = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (frame == NULL) { ESP_LOGW(TAG, "Remote frame allocation failed: %dx%d", w, h); return NULL; }
    s_capture_pixels = 0;
    s_capture = frame;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(s_disp);
    s_capture = NULL;
    if (s_capture_pixels < (uint32_t)w * (uint32_t)h) {
        ESP_LOGW(TAG, "Incomplete remote frame: %lu/%lu pixels, headless %d",
                 (unsigned long)s_capture_pixels, (unsigned long)((uint32_t)w * (uint32_t)h), s_headless);
        lv_draw_buf_destroy(frame);
        return NULL;
    }
    return frame;
}

/* ------------------------------------------------------ remote mirror */

/*
 * While a remote client watches, frames go only to a PSRAM shadow (RGB565
 * little endian) and the panel sleeps: no QSPI transfer, no forced full
 * redraw per request. Only the area that changed is handed out. The shadow,
 * the dirty area and the lease timer are owned by the lvgl task; the atomics
 * are read by any task.
 */
#define MIRROR_LEASE_MS    4000    /* no client request this long: the panel comes back */
#define MIRROR_TAKEOVER_MS 10000   /* used on the device itself: refuse a new session this long */

static uint16_t *s_mirror;
static lv_area_t s_mirror_dirty;
static bool s_mirror_has_dirty;
static _Atomic bool s_mirror_on;
static _Atomic uint32_t s_mirror_seq;
static _Atomic int64_t s_mirror_lease_us;
static _Atomic int64_t s_mirror_takeover_us;
static lv_timer_t *s_mirror_timer;
static void (*s_mirror_stop_cb)(void);

/* lv_area intersect/join are private in LVGL 9: small local versions. */
static bool area_clip(lv_area_t *out, const lv_area_t *a, const lv_area_t *b)
{
    out->x1 = LV_MAX(a->x1, b->x1);
    out->y1 = LV_MAX(a->y1, b->y1);
    out->x2 = LV_MIN(a->x2, b->x2);
    out->y2 = LV_MIN(a->y2, b->y2);
    return out->x1 <= out->x2 && out->y1 <= out->y2;
}

static void area_grow(lv_area_t *acc, const lv_area_t *a)
{
    acc->x1 = LV_MIN(acc->x1, a->x1);
    acc->y1 = LV_MIN(acc->y1, a->y1);
    acc->x2 = LV_MAX(acc->x2, a->x2);
    acc->y2 = LV_MAX(acc->y2, a->y2);
}

static void mirror_flush(lv_display_t *disp, const lv_area_t *area, const uint8_t *pixels)
{
    int w = lv_display_get_horizontal_resolution(disp);
    int h = lv_display_get_vertical_resolution(disp);
    lv_area_t clip = {0, 0, w - 1, h - 1};
    lv_area_t part;

    if (area_clip(&part, area, &clip))
    {
        int stride = (int)lv_draw_buf_width_to_stride(lv_area_get_width(area), LV_COLOR_FORMAT_RGB565_SWAPPED);

        for (int y = part.y1; y <= part.y2; y++)
        {
            const uint8_t *src = pixels + (size_t)(y - area->y1) * stride + (size_t)(part.x1 - area->x1) * 2;
            uint16_t *dst = s_mirror + (size_t)y * w;

            for (int x = part.x1; x <= part.x2; x++, src += 2)
            {
                dst[x] = (uint16_t)((uint16_t)src[0] << 8 | src[1]);
            }
        }

        if (s_mirror_has_dirty)
        {
            area_grow(&s_mirror_dirty, &part);
        }
        else
        {
            s_mirror_dirty = part;
            s_mirror_has_dirty = true;
        }
    }

    if (lv_display_flush_is_last(disp))
    {
        atomic_fetch_add(&s_mirror_seq, 1);
    }
}

static void mirror_stop(bool takeover)
{
    if (!atomic_load(&s_mirror_on))
    {
        return;
    }

    atomic_store(&s_mirror_on, false);

    if (s_mirror_timer != NULL)
    {
        lv_timer_delete(s_mirror_timer);
        s_mirror_timer = NULL;
    }

    heap_caps_free(s_mirror);
    s_mirror = NULL;
    s_mirror_has_dirty = false;

    if (takeover)
    {
        atomic_store(&s_mirror_takeover_us, esp_timer_get_time());
    }

    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    power_remote_mirror(false);

    if (s_mirror_stop_cb != NULL)
    {
        s_mirror_stop_cb();
    }

    ESP_LOGI(TAG, "Remote mirror off%s", takeover ? " (used on the device)" : "");
}

static void mirror_lease_cb(lv_timer_t *timer)
{
    (void)timer;

    if (esp_timer_get_time() - atomic_load(&s_mirror_lease_us) > (int64_t)MIRROR_LEASE_MS * 1000)
    {
        mirror_stop(false);
    }
}

bool lv_port_mirror_active(void)
{
    return atomic_load(&s_mirror_on);
}

void lv_port_mirror_set_stop_cb(void (*cb)(void))
{
    s_mirror_stop_cb = cb;
}

void lv_port_mirror_lease(void)
{
    atomic_store(&s_mirror_lease_us, esp_timer_get_time());
}

uint32_t lv_port_mirror_seq(void)
{
    return atomic_load(&s_mirror_seq);
}

esp_err_t lv_port_mirror_start(void)
{
    lv_port_mirror_lease();

    if (atomic_load(&s_mirror_on))
    {
        return ESP_OK;
    }

    int64_t takeover = atomic_load(&s_mirror_takeover_us);

    if (takeover != 0 && esp_timer_get_time() - takeover < (int64_t)MIRROR_TAKEOVER_MS * 1000)
    {
        return ESP_ERR_INVALID_STATE;
    }

    int w = lv_display_get_horizontal_resolution(s_disp);
    int h = lv_display_get_vertical_resolution(s_disp);

    s_mirror = heap_caps_calloc((size_t)w * h, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_mirror_timer = s_mirror != NULL ? lv_timer_create(mirror_lease_cb, 500, NULL) : NULL;

    if (s_mirror_timer == NULL)
    {
        heap_caps_free(s_mirror);
        s_mirror = NULL;
        ESP_LOGW(TAG, "Remote mirror allocation failed: %dx%d", w, h);
        return ESP_ERR_NO_MEM;
    }

    s_mirror_has_dirty = false;
    atomic_store(&s_mirror_on, true);
    power_remote_mirror(true);

    /* Fill the whole shadow once; afterwards only changes are drawn. */
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    ESP_LOGI(TAG, "Remote mirror on: panel off, %dx%d shadow", w, h);

    return ESP_OK;
}

void lv_port_mirror_stop(bool takeover)
{
    mirror_stop(takeover);
}

lv_draw_buf_t *lv_port_mirror_take(bool full, lv_area_t *area)
{
    if (!atomic_load(&s_mirror_on) || s_mirror == NULL)
    {
        return NULL;
    }

    int w = lv_display_get_horizontal_resolution(s_disp);
    int h = lv_display_get_vertical_resolution(s_disp);
    lv_area_t part = {0, 0, w - 1, h - 1};

    if (!full)
    {
        if (!s_mirror_has_dirty)
        {
            return NULL;
        }

        part = s_mirror_dirty;
    }

    int pw = lv_area_get_width(&part);
    int ph = lv_area_get_height(&part);
    lv_draw_buf_t *out = lv_draw_buf_create(pw, ph, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);

    if (out == NULL)
    {
        return NULL;
    }

    for (int y = 0; y < ph; y++)
    {
        memcpy(out->data + (size_t)y * out->header.stride, s_mirror + (size_t)(part.y1 + y) * w + part.x1, (size_t)pw * 2);
    }

    s_mirror_has_dirty = false;
    *area = part;

    return out;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *pixels)
{
    capture_flush(area, pixels);
    if (s_mirror != NULL)
    {
        mirror_flush(disp, area, pixels);
        lv_display_flush_ready(disp);   /* the panel sleeps: nothing to wait for */
        return;
    }
    if (s_headless)
    {
        lv_display_flush_ready(disp);
        return;
    }
    display_draw(area->x1, area->y1, area->x2, area->y2, pixels);
}

/* Instead of spinning while the DMA works, sleep until its interrupt. */
static void flush_wait_cb(lv_display_t *disp)
{
    (void)disp;

    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        atomic_fetch_add(&s_flush_timeouts, 1);   /* the DMA "done" never came: the frame is late */
    }
}

/* Panels only accept some window alignments: the hardware layer knows which. */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *area = lv_event_get_invalidated_area(e);
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2;
    int y2 = area->y2;

    display_round_area(&x1, &y1, &x2, &y2);

    area->x1 = (int32_t)x1;
    area->y1 = (int32_t)y1;
    area->x2 = (int32_t)x2;
    area->y2 = (int32_t)y2;
}

/* --------------------------------------------------------------- touch */

static void touch_irq(void *ctx)
{
    (void)ctx;

    s_touch_irq = true;

    /* A GPIO interrupt, or a driver task when INT comes through an IO expander. */
    if (xPortInIsrContext())
    {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_wake, &woken);
        portYIELD_FROM_ISR(woken);
    }
    else
    {
        xSemaphoreGive(s_wake);
    }
}

static void stop_touch_polling(void)
{
    lv_timer_pause(lv_indev_get_read_timer(s_indev));
    if (!s_headless) touch_irq_arm();
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    uint16_t x = 0;
    uint16_t y = 0;
    bool pressed;
    uint32_t fake = atomic_load(&s_fake_touch);

    if (fake != 0)
    {
        pressed = (fake & FAKE_PRESSED) != 0;
        x = (uint16_t)((fake >> 12) & 0xFFF);
        y = (uint16_t)(fake & 0xFFF);

        if (!pressed)
        {
            atomic_compare_exchange_strong(&s_fake_touch, &fake, 0);   /* released once, then the real chip */
        }
    }
    else
    {
        pressed = !s_headless && touch_read(&x, &y);

        if (pressed && s_mirror != NULL)
        {
            /* Someone uses the device itself: give the panel back, swallow this touch. */
            mirror_stop(true);
            s_swallow_until_release = true;
        }
    }

    if (!pressed)
    {
        data->state = LV_INDEV_STATE_RELEASED;
        s_was_pressed = false;
        s_edge_candidate = false;
        s_swallow_until_release = false;

        /* A few quiet reads in a row: stop polling, the INT pin takes over. */
        if (++s_released_reads >= 3 && (s_headless || !touch_pin_active()))
        {
            s_released_reads = 0;
            stop_touch_polling();
        }

        return;
    }

    s_released_reads = 0;
    power_user_activity();

    if (!s_was_pressed)
    {
        s_was_pressed = true;
        s_start_x = (int16_t)x;
        s_start_y = (int16_t)y;
        /* Only where "back" makes sense: elsewhere the touch is left alone. */
        s_edge_candidate = x < EDGE_ZONE_PX && (s_back_allowed == NULL || s_back_allowed());
    }
    else if (s_edge_candidate && (int)x - s_start_x > EDGE_SWIPE_DX &&
             abs((int)y - s_start_y) < EDGE_SWIPE_MAX_DY)
    {
        s_edge_candidate = false;
        s_back_gesture = true;
        s_swallow_until_release = true;
    }

    if (s_swallow_until_release)
    {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    data->point.x = (int32_t)x;
    data->point.y = (int32_t)y;
    data->state = LV_INDEV_STATE_PRESSED;
}

/* ---------------------------------------------------------------- knob */

static void knob_cb(int delta)
{
    atomic_fetch_add(&s_knob_delta, delta);
    lv_port_wake();
}

/* The first visible object (depth first) that can still scroll vertically. */
static lv_obj_t *find_scrollable(lv_obj_t *obj, int depth)
{
    if (obj == NULL || lv_obj_is_hidden(obj))
    {
        return NULL;
    }

    if (lv_obj_is_scrollable(obj) && (lv_obj_get_scroll_dir(obj) & LV_DIR_VER) &&
        (lv_obj_get_scroll_top(obj) > 0 || lv_obj_get_scroll_bottom(obj) > 0))
    {
        return obj;
    }

    if (depth >= KNOB_SEARCH_DEPTH)
    {
        return NULL;
    }

    uint32_t count = lv_obj_get_child_count(obj);

    for (uint32_t i = 0; i < count; i++)
    {
        lv_obj_t *found = find_scrollable(lv_obj_get_child(obj, (int32_t)i), depth + 1);

        if (found != NULL)
        {
            return found;
        }
    }

    return NULL;
}

/* ------------------------------------------------------ focus navigation */

/*
 * With focus navigation on, the knob moves a highlight over the interactive
 * objects of the screen (rows with an action, buttons, switches, sliders...)
 * and the knob click "presses" the highlighted one. It is an LVGL encoder
 * input device; the group of focusable objects is rebuilt from the screen
 * whenever it changes, so apps need nothing special: widgets created with
 * the ui helpers are marked (lv_port_set_focusable), LVGL's own controls
 * are recognised by their class.
 */

static bool is_focus_target(lv_obj_t *obj)
{
    if (!lv_obj_is_clickable(obj) || lv_obj_is_disabled(obj))
    {
        return false;
    }

    if (lv_obj_get_user_flag(obj, LV_PORT_FOCUSABLE_FLAG))
    {
        return true;
    }

    return lv_obj_check_type(obj, &lv_button_class) || lv_obj_check_type(obj, &lv_switch_class) ||
           lv_obj_check_type(obj, &lv_slider_class) || lv_obj_check_type(obj, &lv_checkbox_class) ||
           lv_obj_check_type(obj, &lv_dropdown_class) || lv_obj_check_type(obj, &lv_roller_class) ||
           lv_obj_check_type(obj, &lv_textarea_class) || lv_obj_check_type(obj, &lv_buttonmatrix_class) ||
           lv_obj_check_type(obj, &lv_keyboard_class);
}

/* Focusable objects under `obj`, in screen order. Pages scrolled out sideways are skipped. */
static void collect_targets(lv_obj_t *obj, lv_obj_t **list, uint32_t *count, int32_t width)
{
    uint32_t children = lv_obj_get_child_count(obj);

    for (uint32_t i = 0; i < children && *count < FOCUS_MAX; i++)
    {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        lv_area_t area;

        if (lv_obj_is_hidden(child))
        {
            continue;
        }

        lv_obj_get_coords(child, &area);

        if (area.x2 < 0 || area.x1 >= width)
        {
            continue;   /* another page of a horizontal pager (home screen) */
        }

        /* A tile view (home screen): only the tile on show counts. */
        if (lv_obj_check_type(obj, &lv_tileview_class) && child != lv_tileview_get_tile_active(obj))
        {
            continue;
        }

        if (is_focus_target(child))
        {
            list[(*count)++] = child;
            continue;
        }

        collect_targets(child, list, count, width);
    }
}

static void style_focus(lv_obj_t *obj)
{
    /* Local styles: setting them again on every rebuild changes nothing. */
    lv_obj_set_style_outline_width(obj, 3, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(obj, 3, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(obj, s_focus_color, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(obj, lv_color_hex(0xFF9F0A), LV_STATE_FOCUS_KEY | LV_STATE_EDITED);

    /* Scrolling to the focused object is ours (centred, see on_group_focus), not LVGL's. */
    lv_obj_set_scroll_on_focus(obj, false);
}

/* A dialog on the top layer takes the focus; otherwise the active screen. */
static lv_obj_t *focus_root(void)
{
    lv_obj_t *top = lv_layer_top();
    uint32_t count = lv_obj_get_child_count(top);

    for (uint32_t i = 0; i < count; i++)
    {
        if (!lv_obj_is_hidden(lv_obj_get_child(top, (int32_t)i)))
        {
            return top;
        }
    }

    return lv_screen_active();
}

static void sync_focus_group(void)
{
    lv_obj_t *list[FOCUS_MAX];
    uint32_t count = 0;
    lv_obj_t *root = focus_root();

    s_focus_root = root;
    s_focus_layer_count = lv_obj_get_child_count(lv_layer_top());

    lv_obj_update_layout(root);
    collect_targets(root, list, &count, lv_display_get_horizontal_resolution(s_disp));

    bool same = count == lv_group_get_obj_count(s_group);

    for (uint32_t i = 0; same && i < count; i++)
    {
        same = lv_group_get_obj_by_index(s_group, i) == list[i];
    }

    if (same)
    {
        return;
    }

    lv_obj_t *focused = lv_group_get_focused(s_group);
    lv_obj_t *keep = NULL;
    lv_obj_t *first_visible = NULL;
    const int32_t height = lv_display_get_vertical_resolution(s_disp);

    lv_group_remove_all_objs(s_group);

    for (uint32_t i = 0; i < count; i++)
    {
        lv_area_t area;
        lv_obj_get_coords(list[i], &area);
        style_focus(list[i]);
        lv_group_add_obj(s_group, list[i]);

        if (list[i] == focused)
        {
            keep = list[i];
        }

        if (first_visible == NULL && area.y1 >= 0 && area.y2 < height)
        {
            first_visible = list[i];
        }
    }

    /* Keep the highlight where it was; else start from what the user is looking at. */
    lv_obj_t *target = keep != NULL ? keep : first_visible;

    if (target != NULL)
    {
        /* Landing on a page must not scroll it: its top stays in view. */
        s_focus_quiet = true;
        lv_group_focus_obj(target);
        s_focus_quiet = false;
    }
}

/*
 * The highlight moved: scroll its list so it sits in the middle of the
 * screen (on a round panel the top and bottom are narrow and cut off).
 * Tile views are left alone: changing tile is the user's choice.
 */
static void on_group_focus(lv_group_t *group)
{
    lv_obj_t *obj = lv_group_get_focused(group);
    lv_obj_t *parent = (obj != NULL && !s_focus_quiet) ? lv_obj_get_parent(obj) : NULL;

    while (parent != NULL)
    {
        if (lv_obj_check_type(parent, &lv_tileview_class) || lv_obj_check_type(parent, &lv_tileview_tile_class))
        {
            return;
        }

        if (lv_obj_is_scrollable(parent) && (lv_obj_get_scroll_dir(parent) & LV_DIR_VER) &&
            (lv_obj_get_scroll_top(parent) > 0 || lv_obj_get_scroll_bottom(parent) > 0))
        {
            break;
        }

        parent = lv_obj_get_parent(parent);
    }

    if (parent == NULL)
    {
        return;
    }

    lv_area_t item;
    lv_area_t view;
    lv_obj_get_coords(obj, &item);
    lv_obj_get_coords(parent, &view);

    int32_t item_mid = (item.y1 + item.y2) / 2;
    int32_t view_mid = (view.y1 + view.y2) / 2;

    if (item_mid != view_mid)
    {
        lv_obj_scroll_by_bounded(parent, 0, view_mid - item_mid, LV_ANIM_ON);
    }
}

static void focus_settle_cb(lv_timer_t *timer)
{
    (void)timer;
    s_focus_timer = NULL;

    if (s_focus_mode)
    {
        sync_focus_group();
    }
}

/* lvgl task, every pass: a new screen or dialog appeared? Rebuild once it has settled. */
static void watch_focus_root(void)
{
    if (!s_focus_mode || s_focus_timer != NULL)
    {
        return;
    }

    if (focus_root() != s_focus_root || lv_obj_get_child_count(lv_layer_top()) != s_focus_layer_count)
    {
        s_focus_root = focus_root();
        s_focus_layer_count = lv_obj_get_child_count(lv_layer_top());
        s_focus_timer = lv_timer_create(focus_settle_cb, FOCUS_SETTLE_MS, NULL);
        lv_timer_set_repeat_count(s_focus_timer, 1);
    }
}

static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    data->enc_diff = (int16_t)s_enc_diff;
    data->state = s_enc_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    s_enc_diff = 0;
}

static void handle_knob(void)
{
    int delta = atomic_exchange(&s_knob_delta, 0);

    if (delta == 0)
    {
        return;
    }

    if (s_mirror != NULL)
    {
        mirror_stop(true);   /* used on the device: the panel comes back */
        return;
    }

    power_screen_t screen = power_screen();

    if (screen == POWER_SCREEN_OFF || screen == POWER_SCREEN_AOD)
    {
        power_wake(POWER_WAKE_BUTTON);   /* turning the knob only wakes the screen */
        return;
    }

    power_user_activity();

    if (!s_running)
    {
        return;
    }

    /* The screen gets the first chance (volume, pickers, pages...). */
    lv_port_knob_t knob = {.delta = delta, .handled = false};
    lv_obj_send_event(lv_screen_active(), (lv_event_code_t)s_knob_event, &knob);

    if (knob.handled)
    {
        return;
    }

    if (s_focus_mode)
    {
        sync_focus_group();
        s_enc_diff += delta;
        lv_indev_read(s_encoder);
        return;
    }

    lv_obj_t *target = find_scrollable(lv_screen_active(), 0);

    if (target != NULL)
    {
        lv_obj_scroll_by_bounded(target, 0, -delta * KNOB_SCROLL_PX, LV_ANIM_ON);
    }
}

/* ------------------------------------------------------------- task */

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void handle_touch_irq(void)
{
    if (!s_touch_irq)
    {
        return;
    }

    s_touch_irq = false;

    power_screen_t screen = power_screen();

    if (screen == POWER_SCREEN_OFF || screen == POWER_SCREEN_AOD)
    {
        /* Screen off or always-on face: the touch only means "wake up". */
        power_touch_wake();
        s_swallow_until_release = true;
    }

    lv_timer_t *read_timer = lv_indev_get_read_timer(s_indev);
    lv_timer_resume(read_timer);
    lv_timer_ready(read_timer);
}

static void run_async_items(void)
{
    async_item_t item;

    while (xQueueReceive(s_async, &item, 0) == pdTRUE)
    {
        item.fn(item.arg);
    }
}

static void lvgl_task(void *arg)
{
    (void)arg;

    bool boosted = false;

    while (true)
    {
        uint32_t wait_ms = LV_NO_TIMER_READY;

        /* Full CPU speed while there is drawing to do. */
        if (!boosted)
        {
            power_cpu_boost(true);
            boosted = true;
        }

        int64_t pass_start = esp_timer_get_time();
        atomic_store(&s_pass_start_us, pass_start);

        lv_lock();

        handle_touch_irq();
        watch_focus_root();
        handle_knob();
        state_apply_pending();
        run_async_items();

        /* Another task wants the screen up to date now (power manager, before lighting up). */
        if (atomic_exchange(&s_refresh_request, false))
        {
            lv_refr_now(s_disp);
            xSemaphoreGive(s_refresh_done);
        }

        if (s_back_gesture)
        {
            s_back_gesture = false;
            lv_indev_reset(s_indev, NULL);
            lv_indev_wait_release(s_indev);

            if (s_back_cb != NULL)
            {
                s_back_cb();
            }
        }

        if (s_running)
        {
            wait_ms = lv_timer_handler();
        }
        else
        {
            /* Not drawing, but keep reading the touch while a finger is down. */
            lv_timer_t *read_timer = lv_indev_get_read_timer(s_indev);

            if (!lv_timer_get_paused(read_timer))
            {
                lv_indev_read(s_indev);
                wait_ms = 30;
            }
        }

        lv_unlock();

        uint32_t pass_ms = (uint32_t)((esp_timer_get_time() - pass_start) / 1000);
        atomic_store(&s_pass_start_us, 0);

        if (pass_ms > atomic_load(&s_max_pass_ms))
        {
            atomic_store(&s_max_pass_ms, pass_ms);
        }

        /* Long pause ahead (no animation running): let the CPU slow down and sleep. */
        if (boosted && (wait_ms == LV_NO_TIMER_READY || wait_ms > BOOST_HOLD_MS))
        {
            power_cpu_boost(false);
            boosted = false;
        }

        TickType_t ticks;

        if (wait_ms == LV_NO_TIMER_READY)
        {
            ticks = portMAX_DELAY;   /* nothing scheduled: sleep until woken */
        }
        else
        {
            if (wait_ms > MAX_SLEEP_MS)
            {
                wait_ms = MAX_SLEEP_MS;
            }

            ticks = pdMS_TO_TICKS(wait_ms);

            if (ticks == 0)
            {
                ticks = 1;
            }
        }

        xSemaphoreTake(s_wake, ticks);
    }
}

/* ------------------------------------------------------------- public */

static esp_err_t init_port(bool headless)
{
    s_headless = headless;
    const uint16_t width = display_width();
    const uint16_t height = display_height();

    s_wake = xSemaphoreCreateBinary();
    s_flush_done = xSemaphoreCreateBinary();
    s_refresh_done = xSemaphoreCreateBinary();
    s_async = xQueueCreate(ASYNC_QUEUE_LEN, sizeof(async_item_t));

    if (s_wake == NULL || s_flush_done == NULL || s_refresh_done == NULL || s_async == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = headless ? ESP_OK : display_init(flush_done_isr, NULL);

    if (err != ESP_OK)
    {
        return err;
    }

    lv_init();
    lv_tick_set_cb(tick_cb);

    /* DMA displays need internal buffers. CPU-copy displays may render in PSRAM. */
    const size_t buffer_bytes = (size_t)width * CONFIG_BOARD_DRAW_BUFFER_LINES * sizeof(uint16_t);
    uint32_t caps = (headless || board_info()->draw_buffers_in_psram)
                        ? MALLOC_CAP_SPIRAM
                        : (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf1 = heap_caps_malloc(buffer_bytes, caps);
    void *buf2 = heap_caps_malloc(buffer_bytes, caps);

    if (buf1 == NULL || buf2 == NULL)
    {
        heap_caps_free(buf1);
        heap_caps_free(buf2);
        ESP_LOGE(TAG, "No RAM for the draw buffers (caps 0x%lx)", (unsigned long)caps);
        return ESP_ERR_NO_MEM;
    }

    s_disp = lv_display_create(width, height);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);   /* panel wants big endian */
    lv_display_set_buffers(s_disp, buf1, buf2, buffer_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    if (!headless)
    {
        lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);
        lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);
    lv_indev_set_display(s_indev, s_disp);
    lv_indev_set_scroll_throw(s_indev, 6);   /* a bit more inertia than default: feels fluid */
    lv_timer_set_period(lv_indev_get_read_timer(s_indev), 15);   /* 66 Hz while a finger is down */

    state_init();

    if (xTaskCreatePinnedToCore(lvgl_task, "lvgl", TASK_STACK, NULL, TASK_PRIORITY, &s_task, TASK_CORE) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    err = headless ? ESP_OK : touch_init(touch_irq, NULL);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Touch init failed: %s", esp_err_to_name(err));
    }

    s_knob_event = lv_event_register_id();

    if (board_info()->has_knob)
    {
        /* Encoder input for focus navigation: read on demand, never polled. */
        s_group = lv_group_create();
        lv_group_set_wrap(s_group, false);
        lv_group_set_focus_cb(s_group, on_group_focus);
        s_encoder = lv_indev_create();
        lv_indev_set_type(s_encoder, LV_INDEV_TYPE_ENCODER);
        lv_indev_set_read_cb(s_encoder, encoder_read_cb);
        lv_indev_set_display(s_encoder, s_disp);
        lv_indev_set_group(s_encoder, s_group);
        lv_timer_pause(lv_indev_get_read_timer(s_encoder));
        s_focus_color = lv_color_hex(0x3D8BFF);
        s_focus_mode = settings_get()->knob_focus;

        if (knob_init(knob_cb) != ESP_OK)
        {
            ESP_LOGW(TAG, "Knob init failed");
        }
    }

    ESP_LOGI(TAG, "LVGL %d.%d.%d, %dx%d, 2 x %u KB draw buffers", lv_version_major(), lv_version_minor(),
             lv_version_patch(), width, height, (unsigned)(buffer_bytes / 1024));

    return ESP_OK;
}

esp_err_t lv_port_init(void)
{
    return init_port(false);
}

esp_err_t lv_port_init_headless(void)
{
    return init_port(true);
}

bool lv_port_headless(void)
{
    return s_headless;
}

lv_display_t *lv_port_display(void)
{
    return s_disp;
}

lv_indev_t *lv_port_indev(void)
{
    return s_indev;
}

void lv_port_lock(void)
{
    lv_lock();
}

void lv_port_unlock(void)
{
    lv_unlock();
    lv_port_wake();
}

void lv_port_wake(void)
{
    if (s_wake != NULL)
    {
        xSemaphoreGive(s_wake);
    }
}

bool ui_async(ui_async_fn_t fn, void *arg)
{
    async_item_t item = {.fn = fn, .arg = arg};

    if (s_async == NULL || xQueueSend(s_async, &item, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        ESP_LOGW(TAG, "ui_async queue full");
        return false;
    }

    lv_port_wake();

    return true;
}

void lv_port_set_running(bool running)
{
    if (running == s_running)
    {
        return;
    }

    lv_lock();
    s_running = running;

    if (running)
    {
        /* Things may have changed while we were not drawing: redraw it all. */
        lv_obj_invalidate(lv_screen_active());
    }

    lv_unlock();
    lv_port_wake();
}

bool lv_port_is_running(void)
{
    return s_running;
}

void lv_port_refresh_now(void)
{
    if (xTaskGetCurrentTaskHandle() == s_task)
    {
        lv_refr_now(s_disp);
        return;
    }

    /*
     * Rendering needs a big stack (layout, text, styles): do it in the lvgl
     * task and wait for it, never in the caller's (small) stack.
     */
    xSemaphoreTake(s_refresh_done, 0);   /* drop a stale "done" */
    atomic_store(&s_refresh_request, true);
    lv_port_wake();

    if (xSemaphoreTake(s_refresh_done, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        ESP_LOGW(TAG, "Refresh took more than 500 ms");
    }
}

void lv_port_fake_touch(bool pressed, int x, int y)
{
    uint32_t value = (pressed ? FAKE_PRESSED : FAKE_RELEASED) | (((uint32_t)x & 0xFFF) << 12) | ((uint32_t)y & 0xFFF);

    atomic_store(&s_fake_touch, value);
    touch_irq(NULL);   /* same path as the INT pin: wakes the screen, starts reading */
}

uint32_t lv_port_stall_ms(void)
{
    int64_t start = atomic_load(&s_pass_start_us);

    return start == 0 ? 0 : (uint32_t)((esp_timer_get_time() - start) / 1000);
}

void lv_port_take_stats(uint32_t *max_pass_ms, uint32_t *flush_timeouts)
{
    *max_pass_ms = atomic_exchange(&s_max_pass_ms, 0);
    *flush_timeouts = atomic_exchange(&s_flush_timeouts, 0);
}

void lv_port_set_back_gesture_cb(void (*cb)(void))
{
    s_back_cb = cb;
}

void lv_port_set_back_gesture_filter(bool (*allowed)(void))
{
    s_back_allowed = allowed;
}

void lv_port_knob_inject(int delta)
{
    knob_cb(delta);
}

lv_event_code_t lv_port_knob_event(void)
{
    return (lv_event_code_t)s_knob_event;
}

void lv_port_set_focus_mode(bool on)
{
    if (s_group == NULL || on == s_focus_mode)
    {
        return;
    }

    s_focus_mode = on;

    if (on)
    {
        sync_focus_group();
    }
    else
    {
        lv_group_set_editing(s_group, false);
        lv_group_remove_all_objs(s_group);
        s_focus_root = NULL;
    }
}

bool lv_port_focus_mode(void)
{
    return s_focus_mode;
}

void lv_port_focus_enter(void)
{
    if (!s_focus_mode || s_encoder == NULL)
    {
        return;
    }

    sync_focus_group();

    /* A short press and release of the encoder: LVGL clicks (or edits) the focused object. */
    s_enc_pressed = true;
    lv_indev_read(s_encoder);
    s_enc_pressed = false;
    lv_indev_read(s_encoder);
}

void lv_port_set_focus_color(lv_color_t color)
{
    s_focus_color = color;
}
