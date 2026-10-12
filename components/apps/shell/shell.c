#include "apps_internal.h"

#include "apps/apps.h"
#include "core/power.h"
#include "core/lv_port.h"
#include "core/state.h"
#include "services/notify.h"
#include "ui/ui.h"

#include <stdio.h>

/*
 * The home screen: a 3 x 3 tile view with the watch face in the middle
 * (see the drawing in apps.h). LVGL animates the swipes and snaps to tiles.
 */

#define FACE_COL 1
#define FACE_ROW 1

static lv_obj_t *s_tiles = NULL;
static int s_last_notif_count = 0;

void apps_show_home_tile(int col, int row)
{
    if (s_tiles == NULL)
    {
        return;
    }

    app_home();
    lv_tileview_set_tile_by_index(s_tiles, (uint32_t)col, (uint32_t)row, LV_ANIM_OFF);
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

    if (active == face)
    {
        return false;   /* already there: the app manager turns the screen off */
    }

    lv_tileview_set_tile_by_index(s_tiles, FACE_COL, FACE_ROW, LV_ANIM_ON);
    return true;
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

    if (active == lv_obj_get_child(tiles, 3))   /* child 3 = app list (see below) */
    {
        launcher_refresh();
    }
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

    watchface_create(face);
    control_center_create(control);
    notifications_create(notes);
    launcher_create(apps);
    now_playing_create(playing);

    lv_tileview_set_tile_by_index(s_tiles, FACE_COL, FACE_ROW, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_tiles, tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_subject_add_observer_obj(state_subject(STATE_NOTIF_VERSION), notifications_changed, s_tiles, NULL);

    lv_obj_add_event_cb(home, on_knob, lv_port_knob_event(), NULL);
    app_manager_set_home(home, home_back);
    app_manager_set_aod(watchface_aod_create);
}
