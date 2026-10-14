#ifndef CORE_APP_H
#define CORE_APP_H

/*
 * Apps and navigation.
 *
 * An app is a constant description (app_t) with a few callbacks. The app
 * manager creates a fresh LVGL screen for it, calls create(), and slides it
 * in. Back (edge swipe, BOOT click) slides it out and calls destroy().
 *
 *     home (watch shell) ─► launcher ─► app_open("radio") ─► app_open("settings.wifi")
 *        ▲                                   │                       │
 *        └───────────── app_home() ◄──────── app_back() ◄────────────┘
 *
 * Sub-pages are just hidden apps (flag APP_FLAG_HIDDEN), opened with an
 * argument: they get animations and back navigation for free.
 *
 * Every callback runs in the lvgl task (see core/lv_port.h).
 *
 * Minimal app:
 *
 *     static void create(lv_obj_t *screen, void *arg)
 *     {
 *         lv_obj_t *label = lv_label_create(screen);
 *         lv_label_set_text(label, "Hello");
 *         lv_obj_center(label);
 *     }
 *
 *     const app_t hello_app = {
 *         .id = "hello", .name = "Hello", .icon = LV_SYMBOL_HOME,
 *         .color = 0x3D8BFF, .create = create,
 *     };
 *
 *     app_register(&hello_app);   // at boot
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "hardware/button.h"

typedef enum
{
    APP_FLAG_HIDDEN          = 1 << 0,   /* not listed in the launcher */
    APP_FLAG_KEEP_SCREEN_ON  = 1 << 1,   /* screen never times out while open */
    APP_FLAG_STAY_ON_WAKE    = 1 << 2,   /* do not return to the watch face after a long sleep */
    APP_FLAG_NO_BACK_GESTURE = 1 << 3,   /* the app uses horizontal swipes itself */
} app_flags_t;

typedef struct app
{
    const char *id;           /* unique, e.g. "radio" or "settings.wifi" */
    const char *name;         /* shown in the launcher and title bars */
    const char *icon;         /* LV_SYMBOL_xxx, or an image path "S:/apps/x/icon.png" */
    uint32_t color;           /* launcher bubble color 0xRRGGBB */
    uint32_t flags;           /* app_flags_t */

    void (*create)(lv_obj_t *screen, void *arg);   /* build the UI */
    void (*destroy)(void);                          /* free your own memory (widgets are deleted for you) */
    void (*resume)(void);                           /* visible again (a sub-page was closed) */
    void (*pause)(void);                            /* a sub-page covers it */
    bool (*back)(void);                             /* return true to handle "back" yourself */
    /* Optional input override, before normal navigation. True consumes the event. */
    bool (*button)(button_id_t button, button_event_t event);
    /* One or two sentences for the app's details page ("" or NULL = none). */
    const char *about;
} app_t;

void app_register(const app_t *app);

size_t app_count(void);
const app_t *app_at(size_t index);
const app_t *app_find(const char *id);

/* Open an app on top of the current one. `arg` is passed to create(). */
bool app_open(const char *id, void *arg);
bool app_open_app(const app_t *app, void *arg);

/* Close the current app (or let it handle back). */
void app_back(void);

/* "Back" from the user (button, gesture): like app_back(), but closing a
 * listed app asks first (see app_manager_set_close_confirm). */
void app_back_user(void);
/* Close the top app now, without asking it (after the user confirmed). */
void app_close_top(void);
/* Close every open app above `keep_id` (all of them if it is not open):
 * opening an app from a dashboard replaces the previous one. */
void app_close_others(const char *keep_id);
/* Shell: show the "close this app?" question; call app_close_top() on yes. */
void app_manager_set_close_confirm(void (*ask)(const app_t *app));

/* Close everything, back to the watch face. */
void app_home(void);

/* NULL while the home screen (watch shell) is shown. */
const app_t *app_current(void);

/*
 * The home screen and the always-on face are provided by the shell app:
 *   home     created once, never deleted
 *   home_back  called on "back" at home (e.g. return to the watch face tile)
 *   aod_create builds the always-on face on the given screen
 */
void app_manager_set_home(lv_obj_t *home, bool (*home_back)(void));
void app_manager_set_aod(void (*aod_create)(lv_obj_t *screen));

/* Told (lvgl task) when the knob double click switched focus navigation on/off. */
void app_manager_set_focus_cb(void (*changed)(bool on));

/* Start the manager (after lv_port_init and power_init). */
void app_manager_init(void);

/* Dispatch a logical button event in the LVGL task (also used by devtools). */
void app_manager_button(button_id_t button, button_event_t event);

#endif
