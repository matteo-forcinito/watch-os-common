#include "core/app.h"

#include "core/lv_port.h"
#include "core/monkey.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/state.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "apps";

#define MAX_APPS        48
#define MAX_STACK       8
#define MAX_RETIRED     16
#define MAX_OPS         4
#define ANIM_MS         220
#define AUTO_HOME_MS    (2 * 60 * 1000)
#define JANITOR_MS      300

/*
 * HOW SCREENS ARE FREED (the reason this file looks the way it does)
 *
 * While a slide animation runs, LVGL keeps pointers to both screens, and
 * lv_obj_delete() does not clear them: deleting a screen during a transition
 * makes the next redraw use freed memory. So:
 *
 *   - a closed screen is only "retired"; it is deleted later, by gc(), when it
 *     is neither shown, nor the previous, nor the loading screen;
 *   - an app's destroy() runs when its screen is really deleted (its widgets
 *     and timers still exist until then, so observers never see freed state);
 *   - navigation requested during a transition (fast double back, an alarm
 *     opening its screen) is queued and done when the transition ends.
 */

typedef struct
{
    const app_t *app;
    lv_obj_t *screen;
} stack_entry_t;

typedef enum
{
    OP_OPEN,
    OP_BACK,
    OP_HOME,
} op_type_t;

typedef struct
{
    op_type_t type;
    const app_t *app;
    void *arg;
} op_t;

static const app_t *s_apps[MAX_APPS];
static size_t s_app_count = 0;

static stack_entry_t s_stack[MAX_STACK];
static int s_depth = 0;

static lv_obj_t *s_retired[MAX_RETIRED];
static int s_retired_count = 0;

static op_t s_ops[MAX_OPS];
static int s_op_count = 0;

static lv_obj_t *s_home = NULL;
static bool (*s_home_back)(void) = NULL;
static void (*s_focus_changed)(bool on) = NULL;
static void (*s_aod_create)(lv_obj_t *screen) = NULL;
static lv_obj_t *s_aod_screen = NULL;
static int64_t s_sleep_started_us = 0;
static lv_timer_t *s_janitor = NULL;

static void run_pending(void);

/* ------------------------------------------------------------ registry */

void app_register(const app_t *app)
{
    if (s_app_count < MAX_APPS && app != NULL)
    {
        s_apps[s_app_count++] = app;
    }
}

size_t app_count(void)
{
    return s_app_count;
}

const app_t *app_at(size_t index)
{
    return index < s_app_count ? s_apps[index] : NULL;
}

const app_t *app_find(const char *id)
{
    for (size_t i = 0; i < s_app_count; i++)
    {
        if (strcmp(s_apps[i]->id, id) == 0)
        {
            return s_apps[i];
        }
    }

    return NULL;
}

/* -------------------------------------------------- screen life cycle */

static bool transitioning(void)
{
    return lv_display_get_screen_loading(lv_display_get_default()) != NULL;
}

static lv_obj_t *top_screen(void)
{
    return s_depth > 0 ? s_stack[s_depth - 1].screen : s_home;
}

/* Delete the retired screens LVGL no longer refers to. */
static void gc(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_obj_t *active = lv_display_get_screen_active(disp);
    lv_obj_t *prev = lv_display_get_screen_prev(disp);
    lv_obj_t *loading = lv_display_get_screen_loading(disp);

    for (int i = 0; i < s_retired_count;)
    {
        lv_obj_t *screen = s_retired[i];

        if (screen == active || screen == prev || screen == loading)
        {
            i++;
            continue;
        }

        s_retired[i] = s_retired[--s_retired_count];
        lv_obj_delete(screen);   /* -> screen_deleted() -> app destroy() */
    }
}

static void retire(lv_obj_t *screen)
{
    if (s_retired_count == MAX_RETIRED)
    {
        gc();
    }

    if (s_retired_count < MAX_RETIRED)
    {
        s_retired[s_retired_count++] = screen;
    }
    else
    {
        ESP_LOGE(TAG, "Too many screens waiting to be freed");
    }
}

static void janitor_cb(lv_timer_t *timer)
{
    (void)timer;
    run_pending();
}

/* Make sure gc() and the queue run soon, even if no "loaded" event comes. */
static void kick(void)
{
    if (s_janitor == NULL)
    {
        s_janitor = lv_timer_create(janitor_cb, JANITOR_MS, NULL);
    }
    else
    {
        lv_timer_resume(s_janitor);
    }
}

static void after_transition_async(void *arg)
{
    (void)arg;
    run_pending();
}

/*
 * A screen finished loading. LVGL still uses the previous screen after this
 * event, so the cleanup runs a moment later (lv_async_call).
 */
/* When the last app_open() started: its screen is logged once on screen. */
static int64_t s_open_started_us = 0;

static void screen_loaded(lv_event_t *e)
{
    (void)e;

    if (s_open_started_us != 0)
    {
        ESP_LOGI(TAG, "On screen after %d ms", (int)((esp_timer_get_time() - s_open_started_us) / 1000));
        s_open_started_us = 0;
    }

    lv_async_call(after_transition_async, NULL);
}

static void screen_deleted(lv_event_t *e)
{
    const app_t *app = lv_event_get_user_data(e);

    if (app != NULL && app->destroy != NULL)
    {
        app->destroy();
    }
}

static lv_obj_t *new_screen(const app_t *app)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_style_text_font(screen, lv_theme_get_font_normal(screen), 0);   /* the UI font, with accents */
    lv_obj_set_size(screen, LV_PCT(100), LV_PCT(100));
    lv_obj_add_event_cb(screen, screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(screen, screen_deleted, LV_EVENT_DELETE, (void *)app);
    return screen;
}

static void show(lv_obj_t *screen, lv_screen_load_anim_t anim)
{
    if (s_aod_screen != NULL)
    {
        return;   /* always-on face in front: shown when the screen wakes up */
    }

    if (anim == LV_SCREEN_LOAD_ANIM_NONE)
    {
        lv_screen_load(screen);
    }
    else
    {
        lv_screen_load_anim(screen, anim, ANIM_MS, 0, false);
    }

    kick();
}

/* --------------------------------------------------------- operations */

static void close_entry(const stack_entry_t *entry)
{
    if (entry->app->flags & APP_FLAG_KEEP_SCREEN_ON)
    {
        power_keep_screen_on(false);
    }

    retire(entry->screen);
}

static int stack_index(const app_t *app)
{
    for (int i = 0; i < s_depth; i++)
    {
        if (s_stack[i].app == app)
        {
            return i;
        }
    }

    return -1;
}

/*
 * One instance per app: apps keep their widgets in static variables, so a
 * second instance would take them over and the first one's destroy() (which
 * runs late, when its screen is deleted) would free the second one's timers
 * while the first one's keep writing into deleted labels. Opening an app
 * that is already in the stack closes it (and what is above it) first, and
 * deletes its screen now, so its destroy() runs before the new create().
 */
static bool make_room_for(const app_t *app, void *arg)
{
    int index = stack_index(app);

    if (index < 0)
    {
        return true;
    }

    if (index == s_depth - 1 && arg == NULL)
    {
        return false;   /* already on screen: nothing to do */
    }

    if (index == s_depth - 1)
    {
        ESP_LOGW(TAG, "%s is on screen: not opened again", app->id);
        return false;   /* its screen is shown: it cannot be deleted before the new one exists */
    }

    ESP_LOGW(TAG, "%s already open: the old one is closed first", app->id);

    while (s_depth > index)
    {
        stack_entry_t top = s_stack[--s_depth];
        close_entry(&top);
    }

    gc();   /* not shown and no transition running: deleted now -> destroy() */

    return true;
}

static void do_open(const app_t *app, void *arg)
{
    if (!monkey_app_allowed(app->id) || !make_room_for(app, arg))
    {
        return;
    }

    if (s_depth >= MAX_STACK)
    {
        ESP_LOGW(TAG, "Navigation stack full");
        return;
    }

    if (s_depth > 0 && s_stack[s_depth - 1].app->pause != NULL)
    {
        s_stack[s_depth - 1].app->pause();
    }

    s_open_started_us = esp_timer_get_time();

    lv_obj_t *screen = new_screen(app);
    s_stack[s_depth].app = app;
    s_stack[s_depth].screen = screen;
    s_depth++;

    app->create(screen, arg);

    if (app->flags & APP_FLAG_KEEP_SCREEN_ON)
    {
        power_keep_screen_on(true);
    }

    ESP_LOGI(TAG, "Open %s (create %d ms)", app->id, (int)((esp_timer_get_time() - s_open_started_us) / 1000));
    show(screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static void do_back(void)
{
    if (s_depth == 0)
    {
        if (s_home_back == NULL || !s_home_back())
        {
            power_sleep_now();   /* back at the watch face: like lowering the wrist */
        }

        return;
    }

    const app_t *app = s_stack[s_depth - 1].app;

    if (app->back != NULL && app->back())
    {
        return;
    }

    stack_entry_t top = s_stack[--s_depth];
    close_entry(&top);
    show(top_screen(), LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);

    if (s_depth > 0 && s_stack[s_depth - 1].app->resume != NULL)
    {
        s_stack[s_depth - 1].app->resume();
    }
}

static void close_all(void)
{
    while (s_depth > 0)
    {
        stack_entry_t top = s_stack[--s_depth];
        close_entry(&top);
    }
}

static void do_home(void)
{
    if (s_depth == 0)
    {
        if (s_home_back != NULL)
        {
            s_home_back();
        }

        return;
    }

    close_all();
    show(s_home, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

static void execute(const op_t *op)
{
    switch (op->type)
    {
        case OP_OPEN:
            do_open(op->app, op->arg);
            break;

        case OP_BACK:
            do_back();
            break;

        case OP_HOME:
            do_home();
            break;
    }
}

/* Free what can be freed, then do the queued navigation (one transition at a time). */
static void run_pending(void)
{
    gc();

    while (s_op_count > 0 && !transitioning())
    {
        op_t op = s_ops[0];
        memmove(&s_ops[0], &s_ops[1], sizeof(op_t) * (size_t)(s_op_count - 1));
        s_op_count--;
        execute(&op);
    }

    if (s_janitor != NULL && s_op_count == 0 && s_retired_count == 0)
    {
        lv_timer_pause(s_janitor);
    }
}

static bool request(op_type_t type, const app_t *app, void *arg)
{
    op_t op = {.type = type, .app = app, .arg = arg};

    if (!transitioning() && s_op_count == 0)
    {
        gc();
        execute(&op);
        return true;
    }

    if (s_op_count == MAX_OPS)
    {
        ESP_LOGW(TAG, "Navigation queue full, request dropped");
        return false;
    }

    s_ops[s_op_count++] = op;
    kick();

    return true;
}

/* ------------------------------------------------------------- public */

bool app_open_app(const app_t *app, void *arg)
{
    if (app == NULL || app->create == NULL)
    {
        return false;
    }

    return request(OP_OPEN, app, arg);
}

bool app_open(const char *id, void *arg)
{
    const app_t *app = app_find(id);

    if (app == NULL)
    {
        ESP_LOGW(TAG, "No app \"%s\"", id);
        return false;
    }

    return app_open_app(app, arg);
}

void app_back(void)
{
    /* A burst of "back" during an animation counts once per queued slot. */
    request(OP_BACK, NULL, NULL);
}

void app_home(void)
{
    request(OP_HOME, NULL, NULL);
}

const app_t *app_current(void)
{
    return s_depth > 0 ? s_stack[s_depth - 1].app : NULL;
}

/* -------------------------------------------------------- screen states */

static void enter_aod(void)
{
    if (s_aod_screen != NULL || s_aod_create == NULL)
    {
        return;
    }

    lv_obj_t *aod = new_screen(NULL);
    s_aod_create(aod);
    lv_screen_load(aod);   /* finishes any running transition at once */
    s_aod_screen = aod;
    kick();
}

static void leave_aod(void)
{
    if (s_aod_screen == NULL)
    {
        return;
    }

    lv_obj_t *aod = s_aod_screen;
    s_aod_screen = NULL;
    lv_screen_load(top_screen());
    retire(aod);
    kick();
}

static void screen_observer(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;

    power_screen_t screen = (power_screen_t)lv_subject_get_int(subject);

    if (screen == POWER_SCREEN_AOD)
    {
        s_sleep_started_us = esp_timer_get_time();
        enter_aod();
    }
    else if (screen == POWER_SCREEN_OFF)
    {
        if (s_sleep_started_us == 0)
        {
            s_sleep_started_us = esp_timer_get_time();
        }
    }
    else if (screen == POWER_SCREEN_ON)
    {
        /* After a long sleep, wake up on the watch face (like a real watch). */
        bool long_sleep = s_sleep_started_us != 0 &&
                          esp_timer_get_time() - s_sleep_started_us > (int64_t)AUTO_HOME_MS * 1000;
        const app_t *app = app_current();

        if (long_sleep && app != NULL && !(app->flags & APP_FLAG_STAY_ON_WAKE))
        {
            s_op_count = 0;
            close_all();
        }

        if (s_aod_screen != NULL)
        {
            leave_aod();
        }
        else if (lv_screen_active() != top_screen())
        {
            lv_screen_load(top_screen());
            kick();
        }

        if (long_sleep && app_current() == NULL && s_home_back != NULL)
        {
            s_home_back();
        }

        s_sleep_started_us = 0;
    }
}

/* Knob double click: focus navigation on/off, remembered in the settings. */
static void toggle_focus_mode(void)
{
    bool on = !lv_port_focus_mode();
    settings_t settings = *settings_get();

    lv_port_set_focus_mode(on);
    settings.knob_focus = on;
    settings_save(&settings);

    if (s_focus_changed != NULL)
    {
        s_focus_changed(on);
    }
}

static void button_handler(button_id_t button, button_event_t event)
{
    if (event == BUTTON_DOUBLE_CLICK)
    {
        toggle_focus_mode();
        return;
    }

    /* Focus navigation: the click selects, holding goes back. */
    if (lv_port_focus_mode() && button == BUTTON_BOOT)
    {
        if (event == BUTTON_CLICK)
        {
            lv_port_focus_enter();
        }
        else if (event == BUTTON_LONG_PRESS)
        {
            app_back();
        }

        return;
    }

    if (button == BUTTON_BOOT && event == BUTTON_CLICK)
    {
        app_back();
    }
    else if (button == BUTTON_BOOT && event == BUTTON_LONG_PRESS)
    {
        app_home();
    }
    else if (button == BUTTON_PWR && event == BUTTON_CLICK)
    {
        power_sleep_now();
    }
    else if (button == BUTTON_PWR && event == BUTTON_LONG_PRESS)
    {
        app_open("system.power", NULL);
    }
}

static bool back_gesture_allowed(void)
{
    const app_t *app = app_current();
    return app != NULL && !(app->flags & APP_FLAG_NO_BACK_GESTURE);
}

static void back_gesture(void)
{
    const app_t *app = app_current();

    if (app != NULL && !(app->flags & APP_FLAG_NO_BACK_GESTURE))
    {
        app_back();
    }
}

void app_manager_set_focus_cb(void (*changed)(bool on))
{
    s_focus_changed = changed;
}

void app_manager_set_home(lv_obj_t *home, bool (*home_back)(void))
{
    s_home = home;
    s_home_back = home_back;
    lv_obj_add_event_cb(home, screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);
    lv_screen_load(home);
}

void app_manager_set_aod(void (*aod_create)(lv_obj_t *screen))
{
    s_aod_create = aod_create;
}

void app_manager_init(void)
{
    lv_port_lock();
    lv_subject_add_observer(state_subject(STATE_SCREEN), screen_observer, NULL);
    lv_port_unlock();

    lv_port_set_back_gesture_cb(back_gesture);
    lv_port_set_back_gesture_filter(back_gesture_allowed);
    power_set_button_handler(button_handler);
}
