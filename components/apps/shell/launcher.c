#include "apps_internal.h"

#include "core/lv_port.h"
#include "hardware/board.h"
#include "services/extapp.h"
#include "ui/ui.h"

#include <stdlib.h>
#include <string.h>

/*
 * App launcher: a grid of round icons, three per row, in sections.
 *
 *     App                     built-in apps (from the registry)
 *     ( ♪ )  ( ⏰ )  ( ☁ )
 *     Radio  Sveglie Meteo
 *     App esterne             external apps found in /sdcard/apps, with their icons
 *                             (no card: the one app in the app slot)
 *     ( 🖼 )  ( 🖼 )
 *
 * The SD section is rebuilt every time the launcher tile is shown, so a card
 * swapped or an app uploaded from the web page appears without restarting.
 *
 * Round panels get a ring instead, like the hours of a clock: up to 12 apps
 * along the edge, the chosen one big in the middle (touch an icon to open
 * it; with knob focus, turn to go round and click to open). With more apps
 * the 12th place is "Altre", a list of everything.
 */

#define ICON_SIZE   76
#define CELL_WIDTH  104

static lv_obj_t *s_page = NULL;
static lv_obj_t *s_sd_grid = NULL;
static lv_obj_t *s_sd_title = NULL;
static extapp_t *s_ext = NULL;
static int s_ext_count = 0;

/* ---- round panels: the ring */

#define RING_SLOTS    12
#define RING_ICON     58
#define RING_MARGIN   10      /* edge of the glass -> icons */
#define CENTER_ICON   100

typedef struct
{
    const char *icon;
    uint32_t color;
    const char *name;
    const app_t *app;   /* built-in app, or NULL for an external one */
    int ext;            /* index in s_ext */
} entry_t;

static lv_obj_t *s_ring = NULL;          /* holds the icons and the centre */
static lv_obj_t *s_center = NULL;
static lv_obj_t *s_center_name = NULL;
static lv_obj_t *s_center_bubble = NULL;
static entry_t s_entries[RING_SLOTS];
static int s_entry_count = 0;
static int s_selected = 0;

static void ring_build(void);

static lv_obj_t *grid(lv_obj_t *parent)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(g, 14, 0);
    lv_obj_set_scrollable(g, false);
    return g;
}

static lv_obj_t *cell(lv_obj_t *parent, const char *icon, uint32_t color, const char *name, lv_event_cb_t cb,
                      void *user_data)
{
    lv_obj_t *c = lv_obj_create(parent);
    ui_make_card(c);
    lv_obj_set_clickable(c, true);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(c, 2, 0);
    lv_obj_set_size(c, CELL_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_port_set_focusable(c);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *bubble = ui_icon_bubble(c, icon, color, ICON_SIZE);
    lv_obj_set_clickable(bubble, false);

    lv_obj_t *label = lv_label_create(c);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, CELL_WIDTH);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);

    return c;
}

static void open_app(lv_event_t *e)
{
    app_open_app(lv_event_get_user_data(e), NULL);
}

static void open_external(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    if (s_ext == NULL || index >= s_ext_count)
    {
        return;
    }

    extapp_t *copy = malloc(sizeof(extapp_t));   /* owned by the run screen */

    if (copy != NULL)
    {
        *copy = s_ext[index];
        app_open_app(&extapps_run_app, copy);
    }
}

void launcher_refresh(void)
{
    if (s_ring != NULL)
    {
        ring_build();
        return;
    }

    if (s_sd_grid == NULL)
    {
        return;
    }

    if (s_ext == NULL)
    {
        s_ext = calloc(EXTAPP_MAX, sizeof(extapp_t));
    }

    lv_obj_clean(s_sd_grid);
    s_ext_count = s_ext != NULL ? extapp_scan(s_ext, EXTAPP_MAX) : 0;

    for (int i = 0; i < s_ext_count; i++)
    {
        const extapp_t *app = &s_ext[i];
        cell(s_sd_grid, app->icon[0] != '\0' ? app->icon : LV_SYMBOL_DIRECTORY, UI_COLOR_PURPLE, app->name,
             open_external, (void *)(intptr_t)i);
    }

    lv_obj_set_hidden(s_sd_title, s_ext_count == 0);
    lv_obj_set_hidden(s_sd_grid, s_ext_count == 0);
}

/* ================================================================ ring */

static void open_entry(int index)
{
    if (index < 0 || index >= s_entry_count)
    {
        return;
    }

    const entry_t *entry = &s_entries[index];

    if (entry->app != NULL)
    {
        app_open_app(entry->app, NULL);
        return;
    }

    extapp_t *copy = malloc(sizeof(extapp_t));   /* owned by the run screen */

    if (copy != NULL && s_ext != NULL && entry->ext < s_ext_count)
    {
        *copy = s_ext[entry->ext];
        app_open_app(&extapps_run_app, copy);
    }
    else
    {
        free(copy);
    }
}

static void select_entry(int index)
{
    if (index < 0 || index >= s_entry_count || s_center == NULL)
    {
        return;
    }

    s_selected = index;

    if (s_center_bubble != NULL)
    {
        lv_obj_delete(s_center_bubble);
    }

    s_center_bubble = ui_icon_bubble(s_center, s_entries[index].icon, s_entries[index].color, CENTER_ICON);
    lv_obj_move_to_index(s_center_bubble, 0);
    lv_label_set_text(s_center_name, s_entries[index].name);
}

bool launcher_rotate(int delta)
{
    if (s_ring == NULL || s_entry_count == 0 || delta == 0)
    {
        return false;
    }
    int index = (s_selected + delta % s_entry_count) % s_entry_count;
    if (index < 0)
    {
        index += s_entry_count;
    }
    select_entry(index);
    return true;
}

static void on_ring_icon(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED)
    {
        open_entry(index);
    }
    else
    {
        select_entry(index);   /* touched or reached with the knob */
    }
}

static void on_center(lv_event_t *e)
{
    (void)e;
    open_entry(s_selected);
}

static void add_entry(const char *icon, uint32_t color, const char *name, const app_t *app, int ext)
{
    if (s_entry_count < RING_SLOTS)
    {
        s_entries[s_entry_count++] = (entry_t){icon, color, name, app, ext};
    }
}

static void collect_entries(void)
{
    int total = 0;

    for (size_t i = 0; i < app_count(); i++)
    {
        total += !(app_at(i)->flags & APP_FLAG_HIDDEN);
    }

    total += s_ext_count;

    /* More than fit: keep the last place for "Altre". */
    const int room = total > RING_SLOTS ? RING_SLOTS - 1 : RING_SLOTS;
    s_entry_count = 0;

    for (size_t i = 0; i < app_count() && s_entry_count < room; i++)
    {
        const app_t *app = app_at(i);

        if (!(app->flags & APP_FLAG_HIDDEN))
        {
            add_entry(app->icon, app->color, app->name, app, -1);
        }
    }

    for (int i = 0; i < s_ext_count && s_entry_count < room; i++)
    {
        const extapp_t *ext = &s_ext[i];
        add_entry(ext->icon[0] != '\0' ? ext->icon : LV_SYMBOL_DIRECTORY, UI_COLOR_PURPLE, ext->name, NULL, i);
    }

    if (total > RING_SLOTS)
    {
        add_entry(LV_SYMBOL_LIST, UI_COLOR_GRAY, "Altre", &launcher_all_app, -1);
    }
}

static void ring_build(void)
{
    if (s_ext == NULL)
    {
        s_ext = calloc(EXTAPP_MAX, sizeof(extapp_t));
    }

    s_ext_count = s_ext != NULL ? extapp_scan(s_ext, EXTAPP_MAX) : 0;
    collect_entries();

    lv_obj_clean(s_ring);
    s_center_bubble = NULL;

    const int32_t size = board_info()->width;
    const int32_t radius = size / 2 - RING_MARGIN - RING_ICON / 2;

    /* Clockwise from 12 o'clock, spread over the whole circle: also the knob's focus order. */
    const int step = s_entry_count > 0 ? 360 / s_entry_count : 30;

    for (int i = 0; i < s_entry_count; i++)
    {
        int16_t angle = (int16_t)((270 + i * step) % 360);
        int32_t x = size / 2 + ((radius * lv_trigo_cos(angle)) >> LV_TRIGO_SHIFT) - RING_ICON / 2;
        int32_t y = size / 2 + ((radius * lv_trigo_sin(angle)) >> LV_TRIGO_SHIFT) - RING_ICON / 2;

        lv_obj_t *icon = ui_icon_bubble(s_ring, s_entries[i].icon, s_entries[i].color, RING_ICON);
        lv_obj_set_pos(icon, x, y);
        lv_obj_set_clickable(icon, true);
        lv_obj_set_ext_click_area(icon, 6);
        lv_port_set_focusable(icon);
        lv_obj_set_style_transform_pivot_x(icon, RING_ICON / 2, 0);
        lv_obj_set_style_transform_pivot_y(icon, RING_ICON / 2, 0);
        lv_obj_set_style_transform_scale(icon, 280, LV_STATE_PRESSED);
        lv_obj_add_event_cb(icon, on_ring_icon, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(icon, on_ring_icon, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(icon, on_ring_icon, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
    }

    /* The centre: chosen app, big; tap it to open. */
    s_center = lv_obj_create(s_ring);
    lv_obj_remove_style_all(s_center);
    lv_obj_set_size(s_center, 180, 180);   /* corners stay clear of the ring icons */
    lv_obj_center(s_center);
    lv_obj_set_flex_flow(s_center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_center, 10, 0);
    lv_obj_set_scrollable(s_center, false);
    lv_obj_set_clickable(s_center, true);
    lv_obj_add_event_cb(s_center, on_center, LV_EVENT_CLICKED, NULL);

    s_center_name = lv_label_create(s_center);
    lv_obj_set_width(s_center_name, 176);
    lv_label_set_long_mode(s_center_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_center_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_center_name, UI_FONT_LARGE, 0);

    select_entry(s_selected < s_entry_count ? s_selected : 0);
}

static void ring_create(lv_obj_t *parent)
{
    s_ring = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_size(s_ring, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(s_ring, false);
    ring_build();
}

/* ------------------------------------------------- "Altre": every app in a list */

static void all_open(lv_event_t *e)
{
    intptr_t index = (intptr_t)lv_event_get_user_data(e);

    if (index < 0)
    {
        app_open_app(app_at((size_t)(-index - 1)), NULL);
    }
    else if (s_ext != NULL && index < s_ext_count)
    {
        extapp_t *copy = malloc(sizeof(extapp_t));

        if (copy != NULL)
        {
            *copy = s_ext[index];
            app_open_app(&extapps_run_app, copy);
        }
    }
}

static void all_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    /* This page also opens directly from Settings, without visiting the grid. */
    if (s_ext == NULL)
    {
        s_ext = calloc(EXTAPP_MAX, sizeof(extapp_t));
    }
    s_ext_count = s_ext != NULL ? extapp_scan(s_ext, EXTAPP_MAX) : 0;

    lv_obj_t *page = ui_page(screen, "Tutte le app");

    for (size_t i = 0; i < app_count(); i++)
    {
        const app_t *app = app_at(i);

        if (!(app->flags & APP_FLAG_HIDDEN))
        {
            ui_row(page, app->icon, app->color, app->name, NULL, all_open, (void *)(intptr_t)(-(intptr_t)i - 1));
        }
    }

    if (s_ext_count > 0)
    {
        ui_section(page, "APP ESTERNE");
    }

    for (int i = 0; i < s_ext_count; i++)
    {
        ui_row(page, s_ext[i].icon[0] != '\0' ? s_ext[i].icon : LV_SYMBOL_DIRECTORY, UI_COLOR_PURPLE,
               s_ext[i].name, NULL, all_open, (void *)(intptr_t)i);
    }
}

const app_t launcher_all_app = {
    .id = "launcher.all",
    .name = "App",
    .icon = LV_SYMBOL_LIST,
    .color = UI_COLOR_GRAY,
    .flags = APP_FLAG_HIDDEN,
    .create = all_create,
};

/* ================================================================ create */

void launcher_create(lv_obj_t *parent)
{
    if (board_info()->round)
    {
        ring_create(parent);
        return;
    }

    s_page = ui_page(parent, "App");

    lv_obj_t *apps = grid(s_page);

    for (size_t i = 0; i < app_count(); i++)
    {
        const app_t *app = app_at(i);

        if (!(app->flags & APP_FLAG_HIDDEN))
        {
            cell(apps, app->icon, app->color, app->name, open_app, (void *)app);
        }
    }

    s_sd_title = ui_section(s_page, "APP ESTERNE");
    s_sd_grid = grid(s_page);
    launcher_refresh();
}
