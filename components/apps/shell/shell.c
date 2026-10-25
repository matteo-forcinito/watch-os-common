#include "apps_internal.h"

#include "apps/apps.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/lv_port.h"
#include "core/state.h"
#include "services/notify.h"
#include "ui/ui.h"
#if CONFIG_APPKIT_HOSTED
#include "casa.h"
#endif

#include <stdio.h>

/*
 * The home screen: a 3 x 3 tile view with the watch face in the middle
 * (see the drawing in apps.h). LVGL animates the swipes and snaps to tiles.
 */

#define FACE_COL 1
#define FACE_ROW 1

static lv_obj_t *s_tiles = NULL;
static int s_last_notif_count = 0;
static lv_obj_t *s_classic, *s_notifications;
static lv_obj_t *s_casa_home, *s_casa_all;
static lv_obj_t *s_list;           /* homepage "Lista app" */
static bool s_casa_mode, s_list_mode;
#if CONFIG_APPKIT_HOSTED
static void *s_home_component, *s_all_component;
#endif

static void show_bottom(bool notifications)
{
    lv_obj_set_hidden(s_notifications, s_casa_mode && !notifications);
    lv_obj_set_hidden(s_casa_all, !s_casa_mode || notifications);
}

static lv_obj_t *container(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(object, false);
    return object;
}

static void homepage_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;
    bool casa = false;
#if CONFIG_APPKIT_HOSTED
    casa = settings_get()->homepage == 1;
#endif
    bool list = settings_get()->homepage == 2;
    if (list != s_list_mode)
    {
        s_list_mode = list;
        if (list) launcher_list_build(s_list);
        else lv_obj_clean(s_list);
        lv_obj_set_hidden(s_list, !list);
    }
    if (casa == s_casa_mode)
    {
        lv_obj_set_hidden(s_classic, casa || list);
        return;
    }
    s_casa_mode = casa;
    /* Hide the previous face before mounting its replacement. Returning Home
     * only selects this tile; it never launches or reconstructs the Casa app. */
    lv_obj_set_hidden(s_classic, casa || list);
    lv_obj_set_hidden(s_casa_home, !casa);
#if CONFIG_APPKIT_HOSTED
    if (casa)
    {
        if (casa_acquire() == ESP_OK)
        {
            s_home_component = casa_ui_mount(s_casa_home, false);
            s_all_component = casa_ui_mount(s_casa_all, true);
        }
        else ui_text(ui_page(s_casa_home, "Casa"), "Memoria insufficiente", true);
    }
    else
    {
        casa_ui_unmount(s_home_component);
        casa_ui_unmount(s_all_component);
        s_home_component = s_all_component = NULL;
        lv_obj_clean(s_casa_home);
        lv_obj_clean(s_casa_all);
        casa_release();
    }
#endif
    show_bottom(false);
}

void apps_show_home_tile(int col, int row)
{
    if (s_tiles == NULL)
    {
        return;
    }

    app_shell();
    if (col == 1 && row == 2)
    {
        show_bottom(false);
#if CONFIG_APPKIT_HOSTED
        casa_ui_show_all(s_all_component);
#endif
    }
    lv_tileview_set_tile_by_index(s_tiles, (uint32_t)col, (uint32_t)row, LV_ANIM_OFF);
}

void apps_show_notifications(void)
{
    if (!s_tiles) return;
    app_shell();
    show_bottom(true);
    lv_tileview_set_tile_by_index(s_tiles, 1, 2, LV_ANIM_OFF);
}

void shell_show_control_center(void)
{
    if (s_tiles != NULL)
    {
        lv_tileview_set_tile_by_index(s_tiles, FACE_COL, 0, LV_ANIM_ON);
    }
}

/* "Back" on the home screen: return to the watch face. */
static bool home_back(void)
{
    lv_obj_t *active = lv_tileview_get_tile_active(s_tiles);
    lv_obj_t *face = lv_obj_get_child(s_tiles, 0);

    bool centered = active == face && lv_obj_get_scroll_x(s_tiles) == lv_obj_get_x(face) &&
                    lv_obj_get_scroll_y(s_tiles) == lv_obj_get_y(face);
    /* Home also cancels a swipe that has not snapped to its destination yet.
     * Never reveal a partially scrolled shell while closing an app. */
    if (!centered) lv_tileview_set_tile_by_index(s_tiles, FACE_COL, FACE_ROW, LV_ANIM_OFF);
    return !centered;
}

/* New notification while the screen is on: banner at the top. Call: full screen. */
static void notifications_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    notify_call_t call;

    if (notify_get_call(&call) && app_current() != &call_app)
    {
        app_open_app(&call_app, NULL);
    }

    int count = notify_count();

    if (count > s_last_notif_count && power_screen() == POWER_SCREEN_ON)
    {
        notify_t n;

        if (notify_get(0, &n))
        {
            char text[96];
            snprintf(text, sizeof(text), "%.23s: %.70s", n.app[0] != '\0' ? n.app : "Telefono",
                     n.title[0] != '\0' ? n.title : n.body);
            ui_toast(text);
        }
    }

    s_last_notif_count = count;
}

/* Launcher tile shown: pick up apps added to the SD card meanwhile. */
static void tile_changed(lv_event_t *e)
{
    lv_obj_t *tiles = lv_event_get_target_obj(e);
    lv_obj_t *active = lv_tileview_get_tile_active(tiles);
    if (active == lv_obj_get_child(tiles, 0))
    {
        show_bottom(false);
#if CONFIG_APPKIT_HOSTED
        casa_ui_show_all(s_all_component);
#endif
    }

    /* The launcher survives tile changes. Rebuilding it here interrupts the
     * swipe and rescans storage in the LVGL task. Mutations refresh it explicitly. */
    launcher_hint_refresh();
}

static void on_knob(lv_event_t *event)
{
    lv_port_knob_t *knob = lv_event_get_param(event);
    if (knob == NULL || knob->handled || lv_port_focus_mode() || s_tiles == NULL)
    {
        return;
    }
    /* A rotary-only board opens the selected icon by touching the centre. */
    if (lv_tileview_get_tile_active(s_tiles) == lv_obj_get_child(s_tiles, 3))
    {
        knob->handled = launcher_rotate(knob->delta);
    }
}

void shell_create(void)
{
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_remove_style_all(home);
    lv_obj_set_style_bg_color(home, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(home, lv_color_white(), 0);
    lv_obj_set_style_text_font(home, UI_FONT_BODY, 0);

    s_tiles = lv_tileview_create(home);
    lv_obj_set_style_bg_opa(s_tiles, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_tiles, LV_SCROLLBAR_MODE_OFF);

    /* Order matters: child 0 is the watch face (see home_back). */
    lv_obj_t *face = lv_tileview_add_tile(s_tiles, FACE_COL, FACE_ROW, LV_DIR_ALL);
    lv_obj_t *control = lv_tileview_add_tile(s_tiles, FACE_COL, 0, LV_DIR_BOTTOM);
    lv_obj_t *notes = lv_tileview_add_tile(s_tiles, FACE_COL, 2, LV_DIR_TOP);
    lv_obj_t *apps = lv_tileview_add_tile(s_tiles, 2, FACE_ROW, LV_DIR_LEFT);
    lv_obj_t *playing = lv_tileview_add_tile(s_tiles, 0, FACE_ROW, LV_DIR_RIGHT);

    s_classic = container(face);
    s_casa_home = container(face);
    s_notifications = container(notes);
    s_casa_all = container(notes);
    s_list = container(face);
    lv_obj_set_hidden(s_list, true);
    lv_obj_set_hidden(s_casa_home, true);
    lv_obj_set_hidden(s_casa_all, true);
    watchface_create(s_classic);
    control_center_create(control);
    notifications_create(s_notifications);
    launcher_create(apps);
    now_playing_create(playing);

    lv_tileview_set_tile_by_index(s_tiles, FACE_COL, FACE_ROW, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_tiles, tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_subject_add_observer_obj(state_subject(STATE_NOTIF_VERSION), notifications_changed, s_tiles, NULL);

    lv_obj_add_event_cb(home, on_knob, lv_port_knob_event(), NULL);
    /* Observers run immediately: compose the selected face before loading the
     * shell, so even the very first frame cannot expose the classic face. */
    lv_subject_add_observer_obj(state_subject(STATE_SETTINGS_VERSION), homepage_changed, home, NULL);
    app_manager_set_home(home, home_back);
    app_manager_set_aod(watchface_aod_create);
}
