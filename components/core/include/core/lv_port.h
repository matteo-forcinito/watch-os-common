#ifndef CORE_LV_PORT_H
#define CORE_LV_PORT_H

/*
 * LVGL port: connects the LVGL graphics library to the panel and the touch
 * chip, and runs it in its own task ("lvgl").
 *
 * Built to sleep: the task blocks until LVGL really has something to do.
 *
 *   touch INT ──► wake task ──► read touch at 60 Hz while the finger is down
 *                               └─ finger lifted: stop reading, wait for INT
 *   knob turn ──► wake task ──► screen's knob event, else scroll the page
 *   animation / invalidated area ──► LVGL timers ──► render ──► QSPI DMA
 *   nothing to do ──────────────► task blocked, CPU free to light-sleep
 *
 * THREADING RULE: LVGL is not thread safe. UI code runs in the lvgl task
 * (event callbacks, app create/destroy, ui_async() callbacks). Any other
 * task must either wrap LVGL calls in lv_port_lock()/lv_port_unlock() or,
 * better, post work with ui_async() (never blocks, no deadlocks).
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/* Start display, touch, LVGL and the lvgl task. */
esp_err_t lv_port_init(void);

lv_display_t *lv_port_display(void);
lv_indev_t *lv_port_indev(void);

void lv_port_lock(void);
void lv_port_unlock(void);

/* Wake the lvgl task (after changing something from another task). */
void lv_port_wake(void);

/* Run `fn(arg)` in the lvgl task, soon. Safe from any task. */
typedef void (*ui_async_fn_t)(void *arg);
bool ui_async(ui_async_fn_t fn, void *arg);

/*
 * Stop / restart rendering (used by the power manager while the panel is
 * asleep: nothing is drawn, touches are not passed to the UI).
 */
void lv_port_set_running(bool running);
bool lv_port_is_running(void);

/*
 * Render everything that changed right now and wait until it is on the panel.
 * From any task: the work is done in the lvgl task. Do not hold the LVGL lock.
 */
void lv_port_refresh_now(void);

/*
 * Health. lv_port_stall_ms(): how long the lvgl task has been busy in its
 * current pass (0 = waiting for work); a pass of many seconds means the UI is
 * stuck (the power manager then restarts with a core dump instead of a frozen
 * screen). lv_port_take_stats(): slowest pass and DMA "done" interrupts that
 * never came since the last call. Any task.
 */
uint32_t lv_port_stall_ms(void);
void lv_port_take_stats(uint32_t *max_pass_ms, uint32_t *flush_timeouts);

/*
 * Synthetic touch for tests (monkey test): read instead of the touch chip
 * until a "released" has been read. Any task.
 */
void lv_port_fake_touch(bool pressed, int x, int y);

/* Called (in the lvgl task) for a "swipe right from the left edge". */
void lv_port_set_back_gesture_cb(void (*cb)(void));

/*
 * Asked when a touch starts at the left edge: false = no back gesture now,
 * the touch goes to the UI untouched (apps that swipe themselves, home).
 */
void lv_port_set_back_gesture_filter(bool (*allowed)(void));

/*
 * Knob (boards with board_info()->has_knob). Each turn is sent to the active
 * screen as the event lv_port_knob_event(), with a lv_port_knob_t parameter.
 * A screen that uses the knob itself (volume, a picker) sets handled = true:
 *
 *     static void on_knob(lv_event_t *e)
 *     {
 *         lv_port_knob_t *knob = lv_event_get_param(e);
 *         change_volume(knob->delta);
 *         knob->handled = true;
 *     }
 *     lv_obj_add_event_cb(screen, on_knob, lv_port_knob_event(), NULL);
 *
 * Not handled: the first scrollable object on the screen scrolls.
 */
typedef struct
{
    int delta;      /* detents: + clockwise, - counter-clockwise */
    bool handled;
} lv_port_knob_t;

lv_event_code_t lv_port_knob_event(void);

/* Act as if the knob turned `delta` detents (tests, remote control). Any task. */
void lv_port_knob_inject(int delta);

/*
 * Focus navigation (boards with a knob; double click switches it on/off):
 * the knob moves a highlight, the knob click presses the highlighted object.
 * LVGL controls (buttons, switches, sliders...) are found by themselves;
 * custom clickable objects must be marked with lv_port_set_focusable().
 * All in the lvgl task.
 */
#define LV_PORT_FOCUSABLE_FLAG 0   /* lv_obj user flag used for the mark */

static inline void lv_port_set_focusable(lv_obj_t *obj)
{
    lv_obj_set_user_flag(obj, LV_PORT_FOCUSABLE_FLAG, true);
}

void lv_port_set_focus_mode(bool on);
bool lv_port_focus_mode(void);

/* "Press" the highlighted object (knob click in focus mode). */
void lv_port_focus_enter(void);

/* Highlight colour (the theme's accent). */
void lv_port_set_focus_color(lv_color_t color);

#endif
