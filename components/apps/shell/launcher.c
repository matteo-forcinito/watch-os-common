#include "apps_internal.h"

#include "core/lv_port.h"
#include "core/settings.h"
#include "hardware/board.h"
#include "services/extapp.h"
#include "services/fw_version.h"
#include "services/market.h"
#include "ui/ui.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The menu (home screen, swipe left from the watch face).
 *
 *     ★ PREFERITE            up to 6 apps chosen from their action sheet
 *     ( ⏰ ) ( ☁ ) ( ⛽ )
 *     App              13 >  built-in apps, in pages of N cards (Settings > Display)
 *     App esterne       5 >  apps on the card / in the app slot, same pages
 *     Market  2 aggiorn. >
 *     Web server         >
 *     Impostazioni       >
 *     Info               >
 *
 * Round panels show the same entries as a ring (like the hours of a clock):
 * menu entries first, then the favourites; the chosen one big in the middle.
 *
 * In the app pages a tap opens the app's action sheet (Apri, Dettagli,
 * Preferita, and for external apps Aggiorna / Disinstalla); a long press
 * opens the app at once. Pages scroll vertically and snap, so horizontal
 * swipes stay free for "back"; the knob turns the pages.
 */

#define ICON_SIZE   64
#define FAVORITES   6

typedef struct
{
    bool external;
    const app_t *app;          /* built-in */
    extapp_t ext;              /* external */
} launcher_item_t;

enum { MENU_APPS, MENU_EXTERNAL, MENU_MARKET, MENU_WEB, MENU_SETTINGS, MENU_INFO, MENU_COUNT };

static const struct
{
    const char *icon;
    uint32_t color;
    const char *name;
} MENU[MENU_COUNT] = {
    {LV_SYMBOL_LIST, UI_COLOR_BLUE, "App"},
    {LV_SYMBOL_DIRECTORY, UI_COLOR_PURPLE, "App esterne"},
    {LV_SYMBOL_DOWNLOAD, 0x8C7EFF, "Market"},
    {LV_SYMBOL_WIFI, UI_COLOR_TEAL, "Web server"},
    {LV_SYMBOL_SETTINGS, UI_COLOR_GRAY, "Impostazioni"},
    {LV_SYMBOL_EYE_OPEN, UI_COLOR_INDIGO, "Info"},
};

static lv_obj_t *s_menu = NULL;          /* rectangle: rebuilt content of the menu page */
static extapp_t *s_ext = NULL;
static int s_ext_count = 0;

/* ------------------------------------------------------------ helpers */

static void scan_external(void)
{
    if (s_ext == NULL)
    {
        s_ext = heap_caps_calloc(EXTAPP_MAX, sizeof(extapp_t), MALLOC_CAP_SPIRAM);
    }
    s_ext_count = s_ext != NULL ? extapp_scan(s_ext, EXTAPP_MAX) : 0;
}

static int visible_apps(void)
{
    int n = 0;
    for (size_t i = 0; i < app_count(); i++)
    {
        n += !(app_at(i)->flags & APP_FLAG_HIDDEN);
    }
    return n;
}

/* External apps with a newer version in the market catalogue (known after a refresh). */
static int market_updates(void)
{
    market_entry_t *e = heap_caps_malloc(sizeof(*e), MALLOC_CAP_SPIRAM);
    int n = 0;
    for (int i = 0; e != NULL && market_entry(i, e); i++)
    {
        n += e->installed && e->installed_version[0] != '\0' && fw_version_compare(e->version, e->installed_version) > 0;
    }
    free(e);
    return n;
}

static void item_key(const launcher_item_t *item, char *out, size_t size)
{
    if (item->external)
    {
        snprintf(out, size, "ext:%.19s", item->ext.id);
    }
    else
    {
        snprintf(out, size, "%.23s", item->app->id);
    }
}

static bool is_favorite(const launcher_item_t *item)
{
    char key[24];
    item_key(item, key, sizeof(key));
    const settings_t *s = settings_get();
    for (int i = 0; i < FAVORITES; i++)
    {
        if (strcmp(s->favorites[i], key) == 0)
        {
            return true;
        }
    }
    return false;
}

/* A favourite's key -> item. false if the app is gone. */
static bool resolve(const char *key, launcher_item_t *out)
{
    memset(out, 0, sizeof(*out));
    if (strncmp(key, "ext:", 4) == 0)
    {
        for (int i = 0; i < s_ext_count; i++)
        {
            if (strcmp(s_ext[i].id, key + 4) == 0)
            {
                out->external = true;
                out->ext = s_ext[i];
                return true;
            }
        }
        return false;
    }
    out->app = key[0] != '\0' ? app_find(key) : NULL;
    return out->app != NULL;
}

static const char *item_icon(const launcher_item_t *item)
{
    if (item->external)
    {
        return item->ext.icon[0] != '\0' ? item->ext.icon : LV_SYMBOL_DIRECTORY;
    }
    return item->app->icon;
}

static uint32_t item_color(const launcher_item_t *item)
{
    return item->external ? UI_COLOR_PURPLE : item->app->color;
}

static const char *item_name(const launcher_item_t *item)
{
    return item->external ? item->ext.name : item->app->name;
}

static void open_item(const launcher_item_t *item)
{
    if (!item->external)
    {
        app_open_app(item->app, NULL);
        return;
    }
    extapp_t *copy = malloc(sizeof(extapp_t));   /* owned by the run screen */
    if (copy != NULL)
    {
        *copy = item->ext;
        app_open_app(&extapps_open_app, copy);   /* the user chose "open": start at once */
    }
}

static void open_actions(const launcher_item_t *item)
{
    launcher_item_t *copy = malloc(sizeof(*copy));   /* owned by the sheet */
    if (copy != NULL)
    {
        *copy = *item;
        if (!app_open_app(&launcher_actions_app, copy))
        {
            free(copy);
        }
    }
}

static void open_menu_entry(int entry)
{
    switch (entry)
    {
    case MENU_APPS: app_open_app(&launcher_apps_app, (void *)0); break;
    case MENU_EXTERNAL: app_open_app(&launcher_apps_app, (void *)1); break;
    case MENU_MARKET: app_open_app(&market_app, NULL); break;
    case MENU_WEB: app_open_app(&web_app, NULL); break;
    case MENU_SETTINGS: app_open_app(&settings_app, NULL); break;
    default: app_open_app(&settings_about_app, NULL); break;
    }
}

/* ============================================================ the menu */

typedef struct
{
    bool menu;                 /* a menu entry, else a favourite */
    int entry;
    launcher_item_t item;
} slot_t;

#define SLOTS (MENU_COUNT + FAVORITES)

static slot_t *s_slots;
static int s_slot_count;

static void collect_slots(void)
{
    if (s_slots == NULL)
    {
        s_slots = heap_caps_calloc(SLOTS, sizeof(slot_t), MALLOC_CAP_SPIRAM);
        if (s_slots == NULL)
        {
            return;
        }
    }
    s_slot_count = 0;
    for (int i = 0; i < MENU_COUNT; i++)
    {
        s_slots[s_slot_count].menu = true;
        s_slots[s_slot_count].entry = i;
        s_slot_count++;
    }
    const settings_t *s = settings_get();
    for (int i = 0; i < FAVORITES; i++)
    {
        if (resolve(s->favorites[i], &s_slots[s_slot_count].item))
        {
            s_slots[s_slot_count].menu = false;
            s_slot_count++;
        }
    }
}

static void slot_open(int index)
{
    if (s_slots == NULL || index < 0 || index >= s_slot_count)
    {
        return;
    }
    if (s_slots[index].menu)
    {
        open_menu_entry(s_slots[index].entry);
    }
    else
    {
        open_item(&s_slots[index].item);
    }
}

static void on_menu_row(lv_event_t *e)
{
    open_menu_entry((int)(intptr_t)lv_event_get_user_data(e));
}

static void on_favorite(lv_event_t *e)
{
    slot_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void on_favorite_long(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_slots != NULL && index < s_slot_count && !s_slots[index].menu)
    {
        open_actions(&s_slots[index].item);
    }
}

/* ---- rectangle: favourites on top, menu rows below */

static lv_obj_t *small_cell(lv_obj_t *parent, const char *icon, uint32_t color, const char *name, int width)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_set_clickable(c, true);
    lv_obj_set_scrollable(c, false);
    lv_port_set_focusable(c);
    lv_obj_t *bubble = ui_icon_bubble(c, icon, color, ICON_SIZE);
    lv_obj_set_clickable(bubble, false);
    lv_obj_t *label = lv_label_create(c);
    lv_label_set_text(label, name);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
    return c;
}

static void menu_build(void)
{
    lv_obj_clean(s_menu);
    collect_slots();
    if (s_slots == NULL)
    {
        return;
    }
    int favorites = s_slot_count - MENU_COUNT;
    ui_section(s_menu, "PREFERITE");
    if (favorites > 0)
    {
        lv_obj_t *grid = lv_obj_create(s_menu);
        lv_obj_remove_style_all(grid);
        lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(grid, 12, 0);
        lv_obj_set_scrollable(grid, false);
        for (int i = MENU_COUNT; i < s_slot_count; i++)
        {
            const launcher_item_t *item = &s_slots[i].item;
            lv_obj_t *c = small_cell(grid, item_icon(item), item_color(item), item_name(item), 96);
            lv_obj_add_event_cb(c, on_favorite, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(c, on_favorite_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        }
    }
    else
    {
        ui_text(s_menu, "Apri un'app dalle pagine e scegli \"Aggiungi alle preferite\": comparirà qui.", true);
    }

    char value[32];
    for (int i = 0; i < MENU_COUNT; i++)
    {
        const char *v = NULL;
        if (i == MENU_APPS)
        {
            snprintf(value, sizeof(value), "%d", visible_apps());
            v = value;
        }
        else if (i == MENU_EXTERNAL)
        {
            snprintf(value, sizeof(value), "%d", s_ext_count);
            v = value;
        }
        else if (i == MENU_MARKET)
        {
            int updates = market_updates();
            if (updates > 0)
            {
                snprintf(value, sizeof(value), "%d %s", updates, updates == 1 ? "aggiornamento" : "aggiornamenti");
                v = value;
            }
        }
        else if (i == MENU_INFO)
        {
            snprintf(value, sizeof(value), "v%.20s", esp_app_get_description()->version);
            v = value;
        }
        lv_obj_t *row = ui_row(s_menu, MENU[i].icon, MENU[i].color, MENU[i].name, v, on_menu_row, (void *)(intptr_t)i);
        if (i == MENU_MARKET && v != NULL)
        {
            lv_obj_set_style_text_color(ui_row_value(row), lv_color_hex(UI_COLOR_ORANGE), 0);
        }
    }
}

/* ---- round panels: the ring */

#define RING_ICON     58
#define RING_MARGIN   10
#define CENTER_ICON   100

static lv_obj_t *s_ring = NULL;
static lv_obj_t *s_center = NULL;
static lv_obj_t *s_center_name = NULL;
static lv_obj_t *s_center_bubble = NULL;
static int s_selected = 0;

static const char *slot_icon(int i)
{
    return s_slots[i].menu ? MENU[s_slots[i].entry].icon : item_icon(&s_slots[i].item);
}

static uint32_t slot_color(int i)
{
    return s_slots[i].menu ? MENU[s_slots[i].entry].color : item_color(&s_slots[i].item);
}

static const char *slot_name(int i)
{
    return s_slots[i].menu ? MENU[s_slots[i].entry].name : item_name(&s_slots[i].item);
}

static void select_slot(int index)
{
    if (s_slots == NULL || index < 0 || index >= s_slot_count || s_center == NULL)
    {
        return;
    }
    s_selected = index;
    if (s_center_bubble != NULL)
    {
        lv_obj_delete(s_center_bubble);
    }
    s_center_bubble = ui_icon_bubble(s_center, slot_icon(index), slot_color(index), CENTER_ICON);
    lv_obj_move_to_index(s_center_bubble, 0);
    lv_label_set_text(s_center_name, slot_name(index));
}

bool launcher_rotate(int delta)
{
    if (s_ring == NULL || s_slot_count == 0 || delta == 0)
    {
        return false;
    }
    int index = (s_selected + delta % s_slot_count) % s_slot_count;
    select_slot(index < 0 ? index + s_slot_count : index);
    return true;
}

static void on_ring_icon(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED)
    {
        slot_open(index);
    }
    else if (code == LV_EVENT_LONG_PRESSED)
    {
        if (!s_slots[index].menu)
        {
            open_actions(&s_slots[index].item);
        }
    }
    else
    {
        select_slot(index);   /* touched or reached with the knob */
    }
}

static void on_center(lv_event_t *e)
{
    (void)e;
    slot_open(s_selected);
}

static void ring_build(void)
{
    collect_slots();
    lv_obj_clean(s_ring);
    s_center_bubble = NULL;
    if (s_slots == NULL)
    {
        return;
    }
    const int32_t size = board_info()->width;
    const int32_t radius = size / 2 - RING_MARGIN - RING_ICON / 2;
    const int step = s_slot_count > 0 ? 360 / s_slot_count : 30;
    for (int i = 0; i < s_slot_count; i++)
    {
        int16_t angle = (int16_t)((270 + i * step) % 360);
        int32_t x = size / 2 + ((radius * lv_trigo_cos(angle)) >> LV_TRIGO_SHIFT) - RING_ICON / 2;
        int32_t y = size / 2 + ((radius * lv_trigo_sin(angle)) >> LV_TRIGO_SHIFT) - RING_ICON / 2;
        lv_obj_t *icon = ui_icon_bubble(s_ring, slot_icon(i), slot_color(i), RING_ICON);
        lv_obj_set_pos(icon, x, y);
        lv_obj_set_clickable(icon, true);
        lv_obj_set_ext_click_area(icon, 6);
        lv_port_set_focusable(icon);
        lv_obj_set_style_transform_pivot_x(icon, RING_ICON / 2, 0);
        lv_obj_set_style_transform_pivot_y(icon, RING_ICON / 2, 0);
        lv_obj_set_style_transform_scale(icon, 280, LV_STATE_PRESSED);
        if (!s_slots[i].menu)
        {
            /* Favourites wear a thin ring so they read apart from the menu entries. */
            lv_obj_set_style_outline_width(icon, 2, 0);
            lv_obj_set_style_outline_color(icon, lv_color_hex(UI_COLOR_YELLOW), 0);
            lv_obj_set_style_outline_pad(icon, 2, 0);
        }
        lv_obj_add_event_cb(icon, on_ring_icon, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(icon, on_ring_icon, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(icon, on_ring_icon, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(icon, on_ring_icon, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
    }

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
    select_slot(s_selected < s_slot_count ? s_selected : 0);
}

void launcher_refresh(void)
{
    scan_external();
    if (s_ring != NULL)
    {
        ring_build();
    }
    else if (s_menu != NULL)
    {
        menu_build();
    }
}

void launcher_create(lv_obj_t *parent)
{
    scan_external();
    if (board_info()->round)
    {
        s_ring = lv_obj_create(parent);
        lv_obj_remove_style_all(s_ring);
        lv_obj_set_size(s_ring, LV_PCT(100), LV_PCT(100));
        lv_obj_set_scrollable(s_ring, false);
        ring_build();
        return;
    }
    lv_obj_t *page = ui_page(parent, "Menu");
    s_menu = lv_obj_create(page);
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_menu, 8, 0);
    lv_obj_set_scrollable(s_menu, false);
    menu_build();
}

/* ======================================================= the app pages */

typedef struct
{
    bool external;
    launcher_item_t *items;    /* PSRAM */
    int count;
    lv_obj_t *pages;
    lv_obj_t *dots;
    int page_count;
} pages_t;

static void pages_delete(lv_event_t *e)
{
    pages_t *p = lv_event_get_user_data(e);
    free(p->items);
    free(p);
}

static void on_card(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_current_target_obj(e);
    pages_t *p = lv_event_get_user_data(e);
    int index = (int)(intptr_t)lv_obj_get_user_data(card);
    if (index < 0 || index >= p->count)
    {
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
    {
        open_item(&p->items[index]);
    }
    else
    {
        open_actions(&p->items[index]);
    }
}

static void on_pages_scroll(lv_event_t *e)
{
    pages_t *p = lv_event_get_user_data(e);
    int32_t height = lv_obj_get_height(p->pages);
    int page = height > 0 ? (int)((lv_obj_get_scroll_y(p->pages) + height / 2) / height) : 0;
    for (int i = 0; i < (int)lv_obj_get_child_count(p->dots); i++)
    {
        lv_obj_set_style_bg_opa(lv_obj_get_child(p->dots, i), i == page ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

static void open_market(lv_event_t *e)
{
    (void)e;
    app_open_app(&market_app, NULL);
}

static void pages_create(lv_obj_t *screen, void *arg)
{
    pages_t *p = calloc(1, sizeof(*p));
    if (p == NULL)
    {
        return;
    }
    p->external = arg != NULL;
    const board_info_t *b = board_info();
    lv_obj_add_event_cb(screen, pages_delete, LV_EVENT_DELETE, p);

    if (p->external)
    {
        scan_external();
        p->items = heap_caps_calloc(s_ext_count > 0 ? s_ext_count : 1, sizeof(launcher_item_t), MALLOC_CAP_SPIRAM);
        for (int i = 0; p->items != NULL && i < s_ext_count; i++)
        {
            p->items[p->count].external = true;
            p->items[p->count].ext = s_ext[i];
            p->count++;
        }
    }
    else
    {
        p->items = heap_caps_calloc(app_count(), sizeof(launcher_item_t), MALLOC_CAP_SPIRAM);
        for (size_t i = 0; p->items != NULL && i < app_count(); i++)
        {
            if (!(app_at(i)->flags & APP_FLAG_HIDDEN))
            {
                p->items[p->count++].app = app_at(i);
            }
        }
    }

    /* Geometry: a rectangle uses the screen; a round panel the square inside the circle. */
    int cards = settings_get()->launcher_cards;
    int cols = cards == 4 ? 2 : 3, rows = cards == 9 ? 3 : 2;
    int32_t top = b->round ? b->height * 15 / 100 : 52;
    int32_t width = b->round ? b->width * 70 / 100 : b->width - 20;
    int32_t height = b->round ? b->height * 70 / 100 : b->height - top - 10;

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, p->external ? "App esterne" : "App");
    lv_obj_set_style_text_font(title, ui_font_title, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, b->round ? b->height * 4 / 100 : 10);

    if (p->count == 0)
    {
        lv_obj_t *col = lv_obj_create(screen);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, width, LV_SIZE_CONTENT);
        lv_obj_center(col);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 12, 0);
        lv_obj_t *t = ui_text(col, "Nessuna app esterna. Le trovi nel Market.", true);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *m = ui_button(col, LV_SYMBOL_DOWNLOAD "  Market", 0x8C7EFF, open_market, NULL);
        lv_port_set_focusable(m);
        return;
    }

    p->pages = lv_obj_create(screen);
    lv_obj_remove_style_all(p->pages);
    lv_obj_set_size(p->pages, width, height);
    lv_obj_align(p->pages, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_flex_flow(p->pages, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p->pages, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(p->pages, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(p->pages, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(p->pages, on_pages_scroll, LV_EVENT_SCROLL_END, p);

    int32_t cell_w = width / cols, cell_h = height / rows;
    int32_t icon = (cell_w < cell_h ? cell_w : cell_h) * 56 / 100;
    p->page_count = (p->count + cards - 1) / cards;
    for (int pg = 0; pg < p->page_count; pg++)
    {
        lv_obj_t *page = lv_obj_create(p->pages);
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, width, height);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_scrollable(page, false);
        lv_obj_set_snappable(page, true);
        for (int k = 0; k < cards; k++)
        {
            int index = pg * cards + k;
            if (index >= p->count)
            {
                break;
            }
            const launcher_item_t *item = &p->items[index];
            lv_obj_t *card = lv_obj_create(page);
            lv_obj_remove_style_all(card);
            lv_obj_set_size(card, cell_w, cell_h);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(card, 6, 0);
            lv_obj_set_style_radius(card, 18, 0);
            lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_CARD), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_set_scrollable(card, false);
            lv_obj_set_clickable(card, true);
            lv_obj_set_user_data(card, (void *)(intptr_t)index);
            lv_port_set_focusable(card);
            lv_obj_t *bubble = ui_icon_bubble(card, item_icon(item), item_color(item), icon);
            lv_obj_set_clickable(bubble, false);
            lv_obj_t *label = lv_label_create(card);
            lv_label_set_text(label, item_name(item));
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(label, cell_w - 8);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(label, cards == 9 ? UI_FONT_SMALL : UI_FONT_BODY, 0);
            lv_obj_add_event_cb(card, on_card, LV_EVENT_SHORT_CLICKED, p);
            lv_obj_add_event_cb(card, on_card, LV_EVENT_LONG_PRESSED, p);
        }
    }

    /* Page dots on the right (round: inside the circle). */
    p->dots = lv_obj_create(screen);
    lv_obj_remove_style_all(p->dots);
    lv_obj_set_size(p->dots, 10, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p->dots, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p->dots, 6, 0);
    lv_obj_align(p->dots, LV_ALIGN_RIGHT_MID, b->round ? -b->width * 7 / 100 : -1, top / 2);
    for (int i = 0; i < p->page_count && p->page_count > 1; i++)
    {
        lv_obj_t *dot = lv_obj_create(p->dots);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(dot, i == 0 ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

const app_t launcher_apps_app = {
    .id = "launcher.apps",
    .name = "App",
    .icon = LV_SYMBOL_LIST,
    .color = UI_COLOR_BLUE,
    .flags = APP_FLAG_HIDDEN,
    .create = pages_create,
};

/* ===================================================== action sheet */

static launcher_item_t *s_sheet;          /* the open sheet's item (one at a time) */

static void sheet_open(lv_event_t *e)
{
    (void)e;
    if (s_sheet != NULL)
    {
        launcher_item_t item = *s_sheet;
        app_back();
        open_item(&item);
    }
}

static void sheet_details(lv_event_t *e)
{
    (void)e;
    launcher_item_t *copy = s_sheet != NULL ? malloc(sizeof(*copy)) : NULL;
    if (copy != NULL)
    {
        *copy = *s_sheet;
        if (!app_open_app(&launcher_details_app, copy))
        {
            free(copy);
        }
    }
}

static void sheet_favorite(lv_event_t *e)
{
    lv_obj_t *button = lv_event_get_target_obj(e);
    if (s_sheet == NULL)
    {
        return;
    }
    char key[24];
    item_key(s_sheet, key, sizeof(key));
    settings_t s = *settings_get();
    int found = -1, free_slot = -1;
    for (int i = 0; i < FAVORITES; i++)
    {
        if (strcmp(s.favorites[i], key) == 0)
        {
            found = i;
        }
        if (s.favorites[i][0] == '\0' && free_slot < 0)
        {
            free_slot = i;
        }
    }
    if (found >= 0)
    {
        s.favorites[found][0] = '\0';
        ui_toast("Tolta dalle preferite");
    }
    else if (free_slot >= 0)
    {
        snprintf(s.favorites[free_slot], sizeof(s.favorites[free_slot]), "%s", key);
        ui_toast(LV_SYMBOL_OK " Nelle preferite");
    }
    else
    {
        ui_toast("Già 6 preferite: togline una");
        return;
    }
    settings_save(&s);
    lv_label_set_text(lv_obj_get_child(button, 0), found >= 0 ? LV_SYMBOL_PLUS "  Aggiungi alle preferite"
                                                               : LV_SYMBOL_MINUS "  Togli dalle preferite");
    launcher_refresh();
}

static void sheet_update(lv_event_t *e)
{
    (void)e;
    if (s_sheet != NULL && market_install(s_sheet->ext.id) == ESP_OK)
    {
        ui_toast("Aggiornamento avviato");
        app_back();
        app_open_app(&market_app, NULL);
    }
    else
    {
        ui_toast("Aggiornamento non avviato: apri il Market");
    }
}

static void uninstall_confirmed(bool ok, void *data)
{
    (void)data;
    if (!ok || s_sheet == NULL)
    {
        return;
    }
    if (market_remove(s_sheet->ext.id) == ESP_OK)
    {
        ui_toast("Disinstallazione...");
        app_back();
    }
    else
    {
        ui_toast("Non riuscita: apri prima il Market");
    }
}

static void sheet_uninstall(lv_event_t *e)
{
    (void)e;
    if (s_sheet != NULL)
    {
        ui_confirm("Disinstallare?", s_sheet->ext.name, "Disinstalla", UI_COLOR_RED, uninstall_confirmed, NULL);
    }
}

/* Market entry of an external app (false if the catalogue does not list it). */
static bool market_of(const char *id, market_entry_t *out)
{
    for (int i = 0; market_entry(i, out); i++)
    {
        if (strcmp(out->id, id) == 0)
        {
            return true;
        }
    }
    return false;
}

static void sheet_create(lv_obj_t *screen, void *arg)
{
    s_sheet = arg;
    if (s_sheet == NULL)
    {
        return;
    }
    const launcher_item_t *item = s_sheet;
    lv_obj_t *page = ui_page(screen, NULL);
    ui_icon_bubble(page, item_icon(item), item_color(item), 96);
    lv_obj_t *name = ui_title(page, item_name(item));
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    char text[96];
    if (item->external)
    {
        snprintf(text, sizeof(text), "App esterna%s%s", item->ext.version[0] ? " · v" : "", item->ext.version);
    }
    else
    {
        snprintf(text, sizeof(text), "Integrata nel sistema");
    }
    lv_obj_t *sub = ui_text(page, text, true);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *b = ui_button(page, LV_SYMBOL_PLAY "  Apri", UI_COLOR_BLUE, sheet_open, NULL);
    lv_port_set_focusable(b);
    b = ui_button(page, LV_SYMBOL_EYE_OPEN "  Dettagli", 0x2C2C2E, sheet_details, NULL);
    lv_port_set_focusable(b);
    b = ui_button(page, is_favorite(item) ? LV_SYMBOL_MINUS "  Togli dalle preferite" : LV_SYMBOL_PLUS "  Aggiungi alle preferite",
                  0x2C2C2E, sheet_favorite, NULL);
    lv_port_set_focusable(b);
    if (item->external)
    {
        market_entry_t *m = heap_caps_malloc(sizeof(*m), MALLOC_CAP_SPIRAM);
        if (m != NULL && market_of(item->ext.id, m) && item->ext.version[0] != '\0' &&
            fw_version_compare(m->version, item->ext.version) > 0)
        {
            snprintf(text, sizeof(text), LV_SYMBOL_DOWNLOAD "  Aggiorna alla %.20s", m->version);
            b = ui_button(page, text, UI_COLOR_ORANGE, sheet_update, NULL);
            lv_port_set_focusable(b);
        }
        free(m);
        b = ui_button(page, LV_SYMBOL_TRASH "  Disinstalla", 0x3A2228, sheet_uninstall, NULL);
        lv_port_set_focusable(b);
    }
    lv_obj_t *hint = ui_text(page, "Tieni premuta un'app per aprirla subito.", true);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

static void sheet_destroy(void)
{
    free(s_sheet);
    s_sheet = NULL;
}

const app_t launcher_actions_app = {
    .id = "launcher.actions",
    .name = "App",
    .icon = LV_SYMBOL_LIST,
    .color = UI_COLOR_BLUE,
    .flags = APP_FLAG_HIDDEN,
    .create = sheet_create,
    .destroy = sheet_destroy,
};

/* ========================================================== details */

static launcher_item_t *s_details;

static void details_create(lv_obj_t *screen, void *arg)
{
    s_details = arg;
    if (s_details == NULL)
    {
        return;
    }
    const launcher_item_t *item = s_details;
    lv_obj_t *page = ui_page(screen, item_name(item));
    char text[64];
    if (!item->external)
    {
        const app_t *app = item->app;
        ui_text(page, app->about != NULL && app->about[0] != '\0' ? app->about : "App integrata nel sistema.", false);
        ui_section(page, "INFORMAZIONI");
        ui_row(page, LV_SYMBOL_HOME, UI_COLOR_GRAY, "Tipo", "integrata", NULL, NULL);
        snprintf(text, sizeof(text), "v%.20s", esp_app_get_description()->version);
        ui_row(page, LV_SYMBOL_REFRESH, UI_COLOR_GRAY, "Versione", text, NULL, NULL);
        ui_text(page, "Si aggiorna con il sistema (Impostazioni > Aggiornamento).", true);
        return;
    }
    market_entry_t *m = heap_caps_malloc(sizeof(*m), MALLOC_CAP_SPIRAM);
    bool listed = m != NULL && market_of(item->ext.id, m);
    if (listed && m->summary[0] != '\0')
    {
        ui_text(page, m->summary, false);
    }
    ui_section(page, "INFORMAZIONI");
    ui_row(page, LV_SYMBOL_DIRECTORY, UI_COLOR_PURPLE, "Tipo", item->ext.in_slot ? "esterna (memoria app)" : "esterna", NULL, NULL);
    ui_row(page, LV_SYMBOL_OK, UI_COLOR_GREEN, "Installata", item->ext.version[0] ? item->ext.version : "?", NULL, NULL);
    if (listed)
    {
        bool newer = item->ext.version[0] && fw_version_compare(m->version, item->ext.version) > 0;
        ui_row(page, LV_SYMBOL_DOWNLOAD, newer ? UI_COLOR_ORANGE : UI_COLOR_GRAY, "Nel Market", m->version, NULL, NULL);
        snprintf(text, sizeof(text), "%u KB", (unsigned)(m->binary_size / 1024));
        ui_row(page, LV_SYMBOL_SD_CARD, UI_COLOR_GRAY, "Dimensione", text, NULL, NULL);
        if (strcmp(m->channel, "stable") != 0)
        {
            ui_row(page, LV_SYMBOL_WARNING, UI_COLOR_ORANGE, "Canale", m->channel, NULL, NULL);
        }
    }
    else if (!item->ext.in_slot)
    {
        snprintf(text, sizeof(text), "%u KB", (unsigned)(item->ext.size / 1024));
        ui_row(page, LV_SYMBOL_SD_CARD, UI_COLOR_GRAY, "Dimensione", text, NULL, NULL);
        ui_text(page, "Non è nel catalogo del Market (o il catalogo non è ancora stato caricato).", true);
    }
    ui_text(page, "Un'app esterna sostituisce il sistema finché è aperta: premi BOOT per tornare. "
                  "Le sveglie suonano anche lì.", true);
    free(m);
}

static void details_destroy(void)
{
    free(s_details);
    s_details = NULL;
}

const app_t launcher_details_app = {
    .id = "launcher.details",
    .name = "Dettagli",
    .icon = LV_SYMBOL_EYE_OPEN,
    .color = UI_COLOR_GRAY,
    .flags = APP_FLAG_HIDDEN,
    .create = details_create,
    .destroy = details_destroy,
};

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
        launcher_item_t item = {.external = true, .ext = s_ext[index]};
        open_item(&item);
    }
}

static void all_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    scan_external();
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
