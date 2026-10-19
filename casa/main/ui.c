/*
 * Casa UI.
 *
 *   HOME      rooms of tiles. A light that is on glows in its own colour, as
 *             bright as it is. Tap = the natural action (toggle, run, open/close,
 *             play/pause); long press = detail. Summary line on top.
 *   DETAIL    one entity: a big dial (brightness, position, speed, volume,
 *             setpoint) moved by finger or knob, plus its own controls
 *   SETTINGS  setup address and code, connection, exit
 *
 * Tiles are built once per room layout and updated in place when a state
 * arrives (no rebuild under the finger).
 */
#include "casa.h"
#include "sdkconfig.h"

#include "core/app.h"
#include "core/clock.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#if CONFIG_APPKIT_HOSTED
#include "appkit.h"
#else
#include "extapp_sdk.h"
#endif
#include "hardware/board.h"
#include "sdkconfig.h"
#include "ui/ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { HOME, DETAIL, SETTINGS, OVERVIEW, DEVICE };

#define C_BG 0x0A0C10
#define C_TILE 0x15181F
#define C_TILE_PRESSED 0x1D212B
#define C_OFF 0x2A2E38
#define C_INK 0xEEF1F6
#define C_MIST 0x8B93A6
#define C_WARM 0xFFC870
#define C_OK 0x5BD6A0

typedef struct
{
    lv_obj_t *tile, *orb, *glyph, *name, *state;
    char id[CASA_ID];
    bool unit;
} tile_t;

typedef struct
{
    lv_obj_t *screen, *content, *summary, *status;
    lv_timer_t *timer;
    int view;
    tile_t tiles[CASA_ENTITIES];
    int tile_count;
    int group_mode, page, detail_parent;
    bool show_unavailable;
    lv_obj_t *clock_label, *date_label, *category_count[4], *controls, *power_button;
    int clock_minute;
    lv_obj_t *hvac_buttons[6];
    char group[CASA_NAME];
    char groups[CASA_ENTITIES * 4][CASA_NAME];
    int group_count;
    uint32_t revision;
    char layout[CASA_ENTITIES * 8]; /* rooms/ids signature: rebuild only when it changes */
    /* detail */
    char selected[CASA_ID];
    char selected_device[64];
    int unit_heads[CASA_ENTITIES];
    lv_obj_t *dial, *dial_value, *d_state, *d_extra;
    int32_t pending;          /* dial value waiting to be sent, INT32_MIN = none */
    uint32_t pending_at;
    casa_entity_t *list;
    casa_status_t status_now;
} casa_ui_t;

static casa_ui_t *s_ui;
static void render(casa_ui_t *ui);

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_color(color), 0);
    lv_label_set_text(l, text);
    lv_obj_set_clickable(l, false);
    return l;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    return o;
}

static const char *glyph_of(const casa_entity_t *e)
{
    switch (e->kind)
    {
    case CASA_LIGHT: return LV_SYMBOL_CHARGE;
    case CASA_SWITCH: return LV_SYMBOL_POWER;
    case CASA_FAN: return LV_SYMBOL_REFRESH;
    case CASA_COVER: return e->on ? LV_SYMBOL_UP : LV_SYMBOL_DOWN;
    case CASA_CLIMATE: return LV_SYMBOL_SETTINGS;
    case CASA_MEDIA: return e->on ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
    case CASA_SCENE: return LV_SYMBOL_PLAY;
    case CASA_LOCK: return strcmp(e->state, "locked") == 0 ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN;
    case CASA_PERSON: return LV_SYMBOL_HOME;
    case CASA_SENSOR: return LV_SYMBOL_GPS;
    default: return LV_SYMBOL_DUMMY;
    }
}

/* Colour that means "active" for each kind (lights: their own). */
static uint32_t active_colour(const casa_entity_t *e)
{
    switch (e->kind)
    {
    case CASA_LIGHT: return e->rgb ? e->rgb : C_WARM;
    case CASA_SWITCH: return 0x4FB3FF;
    case CASA_FAN: return 0x7FD8E8;
    case CASA_COVER: return 0x9C8CFF;
    case CASA_CLIMATE: return strstr(e->state, "cool") ? 0x4FB3FF : 0xFF8A5B;
    case CASA_MEDIA: return 0xE36FB0;
    case CASA_LOCK: return 0xFF6B6B; /* unlocked = attention */
    case CASA_PERSON: return C_OK;
    case CASA_SCENE: return 0xF7C948;
    default: return 0x4FB3FF;
    }
}

static void find_entity(casa_ui_t *ui, const char *id, casa_entity_t *out, bool *found)
{
    *found = casa_entity(id, out);
    (void)ui;
}

/* ------------------------------------------------------------ home */

static void open_detail(casa_ui_t *ui, const char *id)
{
    snprintf(ui->selected, sizeof(ui->selected), "%s", id);
    ui->detail_parent = ui->view;
    ui->view = DETAIL;
    render(ui);
}

static void tile_event(lv_event_t *e)
{
    tile_t *t = lv_event_get_user_data(e);
    casa_entity_t entity;
    bool found;
    find_entity(s_ui, t->id, &entity, &found);
    if (!found)
        return;
    lv_event_code_t code = lv_event_get_code(e);
    bool read_only = entity.kind == CASA_SENSOR || entity.kind == CASA_PERSON || entity.kind == CASA_OTHER;
    if (code == LV_EVENT_LONG_PRESSED || read_only || entity.kind == CASA_LOCK || entity.kind == CASA_CLIMATE)
    {
        if (code == LV_EVENT_LONG_PRESSED)
            lv_indev_wait_release(lv_indev_active());
        open_detail(s_ui, t->id);
        return;
    }
    if (!casa_tap(&entity))
        ui_toast(s_ui->status_now.connected ? "Un attimo..." : "Home Assistant non collegato");
    else
        lv_obj_set_style_opa(t->orb, 140, 0); /* until the new state arrives */
}

static void update_tile(tile_t *t, const casa_entity_t *e)
{
    char text[64];
    lv_label_set_text(t->name, e->name[0] ? e->name : e->id);
    lv_label_set_text(t->state, casa_state_text(e, text, sizeof(text)));
    lv_obj_set_style_opa(t->orb, LV_OPA_COVER, 0);
    bool active = e->available && e->on && e->kind != CASA_SENSOR;
    uint32_t colour = active_colour(e);
    if (e->kind == CASA_CLIMATE && !isnan(e->current_temp))
    {
        snprintf(text, sizeof(text), "%.0f\xC2\xB0", (double)e->current_temp);
        lv_label_set_text(t->glyph, text);
        lv_obj_set_style_text_font(t->glyph, UI_FONT_SMALL, 0);
    }
    else
    {
        lv_label_set_text(t->glyph, glyph_of(e));
        lv_obj_set_style_text_font(t->glyph, UI_FONT_ICON, 0);
    }
    if (active)
    {
        /* Glow: as strong as the light is bright. */
        int strength = e->kind == CASA_LIGHT && e->brightness >= 0 ? 60 + e->brightness * 195 / 100 : 200;
        lv_obj_set_style_bg_color(t->orb, ui_color(colour), 0);
        lv_obj_set_style_shadow_color(t->orb, ui_color(colour), 0);
        lv_obj_set_style_shadow_width(t->orb, 22, 0);
        lv_obj_set_style_shadow_opa(t->orb, (lv_opa_t)(strength > 255 ? 255 : strength) / 2, 0);
        lv_obj_set_style_text_color(t->glyph, ui_color(0x101216), 0);
        lv_obj_set_style_bg_color(t->tile, lv_color_mix(ui_color(colour), ui_color(C_TILE), 30), 0);
    }
    else
    {
        lv_obj_set_style_bg_color(t->orb, ui_color(C_OFF), 0);
        lv_obj_set_style_shadow_width(t->orb, 0, 0);
        lv_obj_set_style_text_color(t->glyph, ui_color(e->available ? C_MIST : 0x555A66), 0);
        lv_obj_set_style_bg_color(t->tile, ui_color(C_TILE), 0);
    }
    lv_obj_set_style_text_color(t->state, ui_color(!e->available ? 0x666B78 : active ? C_INK : C_MIST), 0);
}

static lv_obj_t *make_tile(casa_ui_t *ui, lv_obj_t *parent, tile_t *t, int w)
{
    t->unit = false;
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, w, 98);
    lv_obj_set_style_radius(tile, 20, 0);
    lv_obj_set_style_bg_color(tile, ui_color(C_TILE), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile, ui_color(C_TILE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(tile, 248, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(tile, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(tile, 49, 0);
    lv_obj_set_style_pad_all(tile, 12, 0);
    lv_obj_set_scrollable(tile, false);
    t->orb = plain(tile);
    lv_obj_set_size(t->orb, 34, 34);
    lv_obj_set_style_radius(t->orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(t->orb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t->orb, ui_color(C_OFF), 0);
    t->glyph = label(t->orb, UI_FONT_ICON, C_MIST, "");
    lv_obj_center(t->glyph);
    t->name = label(tile, UI_FONT_SMALL, C_INK, "");
    lv_obj_set_width(t->name, w - 24);
    lv_label_set_long_mode(t->name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(t->name, lv_font_get_line_height(UI_FONT_SMALL));
    lv_obj_align(t->name, LV_ALIGN_TOP_LEFT, 0, 40);
    t->state = label(tile, UI_FONT_SMALL, C_MIST, "");
    lv_obj_set_width(t->state, w - 24);
    lv_label_set_long_mode(t->state, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(t->state, lv_font_get_line_height(UI_FONT_SMALL));
    lv_obj_align(t->state, LV_ALIGN_TOP_LEFT, 0, 60);
    t->tile = tile;
    lv_obj_add_event_cb(tile, tile_event, LV_EVENT_CLICKED, t);
    lv_obj_add_event_cb(tile, tile_event, LV_EVENT_LONG_PRESSED, t);
    lv_port_set_focusable(tile);
    (void)ui;
    return tile;
}

static void open_settings(lv_event_t *e)
{
    (void)e;
    s_ui->view = SETTINGS;
    render(s_ui);
}

static void summary_text(casa_ui_t *ui, int count, char *out, size_t size)
{
    int available = 0;
    for (int i = 0; i < count; i++) available += ui->list[i].known && ui->list[i].available;
    snprintf(out, size, "%d disponibili / %d selezionati", available, count);
}

static void mode_event(lv_event_t *event)
{
    s_ui->view = HOME;
    s_ui->group_mode = (int)(intptr_t)lv_event_get_user_data(event);
    snprintf(s_ui->group, sizeof(s_ui->group), "%s", s_ui->group_mode == 3 ? "Tutti" : "");
    if (s_ui->group_mode == 3) s_ui->show_unavailable = true;
    s_ui->page = 0;
    render(s_ui);
}

static void group_event(lv_event_t *event)
{
    const char *name = lv_event_get_user_data(event);
    snprintf(s_ui->group, sizeof(s_ui->group), "%s", name);
    s_ui->page = 0;
    render(s_ui);
}

static void page_event(lv_event_t *event)
{
    s_ui->page += (int)(intptr_t)lv_event_get_user_data(event);
    if (s_ui->page < 0) s_ui->page = 0;
    render(s_ui);
}

static const char *entity_group(const casa_entity_t *entity, int index)
{
    if (s_ui->group_mode == 3) return "Tutti";
    if (s_ui->group_mode == 0) return entity->area[0] ? entity->area : "Senza stanza";
    if (s_ui->group_mode == 1) return casa_type_name(entity);
    return entity->dashboards[index][0] ? entity->dashboards[index] : "Senza dashboard";
}

static bool in_group(const casa_entity_t *entity)
{
    int count = s_ui->group_mode == 2 && entity->dashboards[0][0] ? 4 : 1;
    for (int i = 0; i < count; i++)
    {
        if (i && !entity->dashboards[i][0]) break;
        if (!strcmp(entity_group(entity, i), s_ui->group)) return true;
    }
    return false;
}

static void pager(lv_obj_t *page, int count)
{
    char text[48];
    snprintf(text, sizeof(text), "Pagina %d di %d", s_ui->page + 1, (count + 11) / 12);
    ui_text(page, text, true);
    if (s_ui->page) ui_button(page, "Precedenti", C_OFF, page_event, (void *)-1);
    if ((s_ui->page + 1) * 12 < count) ui_button(page, "Successivi", C_OFF, page_event, (void *)1);
}


static void show_overview(lv_event_t *event)
{
    (void)event;
    s_ui->view = OVERVIEW;
    render(s_ui);
}

static void show_category(lv_event_t *event)
{
    s_ui->view = HOME;
    s_ui->group_mode = 1;
    s_ui->page = 0;
    snprintf(s_ui->group, sizeof(s_ui->group), "%s", (const char *)lv_event_get_user_data(event));
    render(s_ui);
}

static void show_offline(lv_event_t *event)
{
    (void)event;
    s_ui->show_unavailable = !s_ui->show_unavailable;
    s_ui->page = 0;
    render(s_ui);
}

static lv_obj_t *panel_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t callback, void *data)
{
    lv_obj_t *button = ui_button(parent, text, color, callback, data);
    lv_obj_set_height(button, 44);
    lv_obj_set_width(button, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_t *text_label = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_font(text_label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(text_label, ui_color(color == C_WARM ? C_BG : C_INK), 0);
    return button;
}

static bool category_matches(const casa_entity_t *entity, int category)
{
    if (category == 0) return entity->kind == CASA_LIGHT;
    if (category == 1) return entity->kind == CASA_CLIMATE;
    if (category == 2) return !strcmp(casa_type_name(entity), "Prese");
    return entity->kind == CASA_SCENE;
}

static void overview_update(casa_ui_t *ui)
{
    int count = casa_entities(ui->list, CASA_ENTITIES);
    char text[128];
    summary_text(ui, count, text, sizeof(text));
    if (ui->summary) lv_label_set_text(ui->summary, text);
    if (ui->status)
    {
        lv_label_set_text(ui->status, ui->status_now.connected ? "Home Assistant  " LV_SYMBOL_OK : ui->status_now.message);
        lv_obj_set_style_text_color(ui->status, ui_color(ui->status_now.connected ? C_OK : C_WARM), 0);
    }
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    if (ui->clock_label && ui->clock_minute != local.tm_min)
    {
        ui->clock_minute = local.tm_min;
        strftime(text, sizeof(text), clock_is_valid() ? "%H:%M" : "--:--", &local);
        lv_label_set_text(ui->clock_label, text);
        static const char *days[] = {"Domenica", "Lunedi", "Martedi", "Mercoledi", "Giovedi", "Venerdi", "Sabato"};
        snprintf(text, sizeof(text), "%s %d/%02d", days[local.tm_wday], local.tm_mday, local.tm_mon + 1);
        lv_label_set_text(ui->date_label, clock_is_valid() ? text : "Sincronizzo l'orologio");
    }
    for (int category = 0; category < 4; category++)
    {
        int total = 0, available = 0, active = 0;
        for (int i = 0; i < count; i++)
            if (category_matches(&ui->list[i], category))
            {
                total++;
                available += ui->list[i].known && ui->list[i].available;
                active += ui->list[i].available && ui->list[i].on;
            }
        if (!total) snprintf(text, sizeof(text), "Da selezionare");
        else if (!available) snprintf(text, sizeof(text), "%d offline in HA", total);
        else if (category == 0 || category == 2) snprintf(text, sizeof(text), "%d accesi / %d online", active, available);
        else snprintf(text, sizeof(text), "%d disponibili", available);
        lv_label_set_text(ui->category_count[category], text);
    }
    for (int tile = 0; tile < ui->tile_count; tile++)
        for (int i = 0; i < count; i++)
            if (!strcmp(ui->tiles[tile].id, ui->list[i].id)) update_tile(&ui->tiles[tile], &ui->list[i]);
}

static void overview(casa_ui_t *ui)
{
    int width = board_info()->round ? board_info()->width * 78 / 100 : board_info()->width - 28;
    lv_obj_t *page = ui_page(ui->content, NULL);
    lv_obj_t *hero = plain(page);
    lv_obj_set_size(hero, width, 174);
    lv_obj_set_style_radius(hero, 22, 0);
    lv_obj_set_style_clip_corner(hero, true, 0);
    lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(hero, ui_color(0x29323D), 0);
    lv_obj_set_style_bg_grad_color(hero, ui_color(0x141B25), 0);
    lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_HOR, 0);
    lv_obj_t *orb = plain(hero);
    lv_obj_set_size(orb, 148, 148);
    lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb, ui_color(C_WARM), 0);
    lv_obj_set_style_bg_grad_color(orb, ui_color(0xE87A35), 0);
    lv_obj_set_style_bg_grad_dir(orb, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(orb, 220, 0);
    lv_obj_align(orb, LV_ALIGN_RIGHT_MID, 58, -10);
    lv_obj_t *brand = label(hero, UI_FONT_SMALL, C_WARM, "CASA");
    lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 18, 12);
    lv_obj_t *settings_button = panel_button(hero, LV_SYMBOL_SETTINGS, C_OFF, open_settings, NULL);
    lv_obj_align(settings_button, LV_ALIGN_TOP_RIGHT, -12, 8);
    ui->clock_label = label(hero, ui_font_big, C_INK, "--:--");
    lv_obj_align(ui->clock_label, LV_ALIGN_TOP_LEFT, 14, 38);
    ui->date_label = label(hero, UI_FONT_SMALL, C_INK, "");
    lv_obj_align(ui->date_label, LV_ALIGN_BOTTOM_LEFT, 18, -32);
    ui->status = label(hero, UI_FONT_SMALL, C_OK, "");
    lv_obj_set_width(ui->status, width - 36);
    lv_label_set_long_mode(ui->status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(ui->status, LV_ALIGN_BOTTOM_LEFT, 18, -10);
    ui->summary = label(page, UI_FONT_SMALL, C_MIST, "");
    lv_obj_t *grid = plain(page);
    lv_obj_set_size(grid, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 10, 0);
    lv_obj_set_style_pad_row(grid, 10, 0);
    static const char *names[] = {"Luci", "Clima", "Prese", "Scene"};
    static const char *groups[] = {"Luci", "Termostati", "Prese", "Scene e azioni"};
    static const char *icons[] = {LV_SYMBOL_CHARGE, LV_SYMBOL_SETTINGS, LV_SYMBOL_POWER, LV_SYMBOL_PLAY};
    static const uint32_t colors[] = {C_WARM, 0xFF9869, 0x69B9FF, 0xB3A0FF};
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *card = plain(grid);
        lv_obj_set_size(card, (width - 10) / 2, 94);
        lv_obj_set_style_bg_color(card, ui_color(C_TILE), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_clickable(card, true);
        lv_obj_add_event_cb(card, show_category, LV_EVENT_CLICKED, (void *)groups[i]);
        lv_port_set_focusable(card);
        lv_obj_t *icon = label(card, UI_FONT_ICON, colors[i], icons[i]);
        lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 14, 12);
        lv_obj_t *name = label(card, UI_FONT_BODY, C_INK, names[i]);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 46, 10);
        ui->category_count[i] = label(card, UI_FONT_SMALL, C_MIST, "");
        lv_obj_set_width(ui->category_count[i], (width - 10) / 2 - 24);
        lv_label_set_long_mode(ui->category_count[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(ui->category_count[i], LV_ALIGN_BOTTOM_LEFT, 14, -14);
    }
    lv_obj_t *navigation = plain(page);
    lv_obj_set_size(navigation, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(navigation, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(navigation, 6, 0);
    panel_button(navigation, "Stanze", C_OFF, mode_event, (void *)0);
    panel_button(navigation, "Tipi", C_OFF, mode_event, (void *)1);
    panel_button(navigation, "Dashboard", C_OFF, mode_event, (void *)2);
    panel_button(navigation, "Tutte", C_OFF, mode_event, (void *)3);
    int count = casa_entities(ui->list, CASA_ENTITIES);
    if (!count) ui_text(page, "Apri Impostazioni per configurare Home Assistant dal PC.", true);
    else
    {
        ui_text(page, "Dispositivi disponibili", false);
        lv_obj_t *devices = plain(page);
        lv_obj_set_size(devices, width, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(devices, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(devices, 10, 0);
        lv_obj_set_style_pad_row(devices, 10, 0);
        for (int priority = 0; priority < 2; priority++)
            for (int i = 0; i < count && ui->tile_count < 6; i++)
            {
                casa_entity_t *entity = &ui->list[i];
                if (!entity->known || !entity->available) continue;
                bool control = entity->kind < CASA_SENSOR;
                if (control != (priority == 0)) continue;
                tile_t *tile = &ui->tiles[ui->tile_count++];
                snprintf(tile->id, sizeof(tile->id), "%s", entity->id);
                make_tile(ui, devices, tile, (width - 10) / 2);
                update_tile(tile, entity);
            }
        if (!ui->tile_count) ui_text(page, "Gli stati non sono ancora disponibili in HA. Puoi consultarli da Stanze o aggiornare la selezione dal PC.", true);
    }
    overview_update(ui);
}


static bool unit_available(casa_ui_t *ui, const char *id)
{
    for (int i = 0; i < ui->status_now.count; i++)
        if (!strcmp(ui->list[i].device_id, id) && ui->list[i].known && ui->list[i].available) return true;
    return false;
}

static void unit_event(lv_event_t *event)
{
    tile_t *tile = lv_event_get_user_data(event);
    snprintf(s_ui->selected_device, sizeof(s_ui->selected_device), "%.63s", tile->id);
    s_ui->view = DEVICE;
    s_ui->page = 0;
    render(s_ui);
}

static void update_unit(casa_ui_t *ui, tile_t *tile)
{
    const casa_entity_t *temperature = NULL, *humidity = NULL, *battery = NULL, *first = NULL;
    int total = 0, available = 0;
    for (int i = 0; i < ui->status_now.count; i++)
    {
        const casa_entity_t *entity = &ui->list[i];
        if (strcmp(entity->device_id, tile->id)) continue;
        if (!first) first = entity;
        total++;
        available += entity->known && entity->available;
        if (!strcmp(entity->device_class, "temperature") && (!temperature || entity->available)) temperature = entity;
        if (!strcmp(entity->device_class, "humidity") && (!humidity || entity->available)) humidity = entity;
        if (!strcmp(entity->device_class, "battery") && !strncmp(entity->id, "sensor.", 7) && (!battery || entity->available)) battery = entity;
    }
    if (!first) return;
    lv_label_set_text(tile->name, first->device_name[0] ? first->device_name : first->name);
    char text[160], t[48] = "--", h[48] = "--", b[48] = "--";
    if (temperature && temperature->available) casa_state_text(temperature, t, sizeof(t));
    if (humidity && humidity->available) casa_state_text(humidity, h, sizeof(h));
    if (battery && battery->available) casa_state_text(battery, b, sizeof(b));
    if (temperature || humidity || battery) snprintf(text, sizeof(text), "T %s  U %s\nBatteria %s", t, h, b);
    else snprintf(text, sizeof(text), "%d / %d disponibili\n%s", available, total, first->area);
    lv_label_set_text(tile->state, text);
    lv_obj_set_style_text_color(tile->state, ui_color(available ? C_MIST : 0x707988), 0);
    lv_obj_set_style_bg_color(tile->orb, ui_color(available ? 0x214956 : C_OFF), 0);
    lv_obj_set_style_text_color(tile->glyph, ui_color(available ? 0x91D8DD : C_MIST), 0);
}

static void make_unit(casa_ui_t *ui, lv_obj_t *parent, tile_t *tile, int width, const casa_entity_t *entity)
{
    make_tile(ui, parent, tile, width);
    tile->unit = true;
    snprintf(tile->id, sizeof(tile->id), "%s", entity->device_id);
    lv_obj_remove_event_cb(tile->tile, tile_event);
    lv_obj_add_event_cb(tile->tile, unit_event, LV_EVENT_CLICKED, tile);
    lv_obj_set_height(tile->tile, 146);
    lv_label_set_text(tile->glyph, LV_SYMBOL_HOME);
    lv_obj_set_height(tile->state, 56);
    lv_label_set_long_mode(tile->state, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(tile->state, LV_ALIGN_TOP_LEFT, 0, 70);
    update_unit(ui, tile);
}

static void all_devices(casa_ui_t *ui, lv_obj_t *page)
{
    int count = casa_entities(ui->list, CASA_ENTITIES), heads = 0;
    for (int i = 0; i < count; i++)
    {
        bool duplicate = false;
        if (ui->list[i].device_id[0])
            for (int j = 0; j < heads; j++)
                if (!strcmp(ui->list[ui->unit_heads[j]].device_id, ui->list[i].device_id)) { duplicate = true; break; }
        if (!duplicate) ui->unit_heads[heads++] = i;
    }
    int width = board_info()->round ? board_info()->width * 78 / 100 : board_info()->width - 28;
    lv_obj_t *grid = plain(page);
    lv_obj_set_size(grid, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 10, 0);
    lv_obj_set_style_pad_row(grid, 10, 0);
    int matched = 0;
    if (ui->page * 12 >= heads) ui->page = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int h = 0; h < heads; h++)
        {
            casa_entity_t *entity = &ui->list[ui->unit_heads[h]];
            bool available = entity->device_id[0] ? unit_available(ui, entity->device_id) : entity->known && entity->available;
            if (available != (pass == 0) || (!available && !ui->show_unavailable)) continue;
            int index = matched++;
            if (index < ui->page * 12 || index >= (ui->page + 1) * 12) continue;
            tile_t *tile = &ui->tiles[ui->tile_count++];
            if (entity->device_id[0]) make_unit(ui, grid, tile, (width - 10) / 2, entity);
            else
            {
                snprintf(tile->id, sizeof(tile->id), "%s", entity->id);
                make_tile(ui, grid, tile, (width - 10) / 2);
                update_tile(tile, entity);
            }
        }
    if (matched > 12) pager(page, matched);
    if (!matched) ui_text(page, "Nessun dispositivo con questi filtri", true);
}

static void device_page(casa_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, NULL);
    int count = casa_entities(ui->list, CASA_ENTITIES), members = 0;
    const casa_entity_t *first = NULL;
    for (int i = 0; i < count; i++)
        if (!strcmp(ui->list[i].device_id, ui->selected_device)) { members++; if (!first) first = &ui->list[i]; }
    if (!first) { ui_text(page, "Dispositivo non trovato", true); return; }
    ui_title(page, first->device_name[0] ? first->device_name : first->name);
    ui_text(page, first->area, true);
    int width = board_info()->round ? board_info()->width * 78 / 100 : board_info()->width - 28;
    lv_obj_t *grid = plain(page);
    lv_obj_set_size(grid, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 10, 0);
    lv_obj_set_style_pad_row(grid, 10, 0);
    int matched = 0;
    if (ui->page * 12 >= members) ui->page = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < count; i++)
        {
            casa_entity_t *entity = &ui->list[i];
            if (strcmp(entity->device_id, ui->selected_device) || (entity->known && entity->available) != (pass == 0)) continue;
            int index = matched++;
            if (index < ui->page * 12 || index >= (ui->page + 1) * 12) continue;
            tile_t *tile = &ui->tiles[ui->tile_count++];
            snprintf(tile->id, sizeof(tile->id), "%s", entity->id);
            make_tile(ui, grid, tile, (width - 10) / 2);
            update_tile(tile, entity);
        }
    if (members > 12) pager(page, members);
}

static void home(casa_ui_t *ui)
{
    const board_info_t *b = board_info();
    int w = b->width;
    lv_obj_t *page = ui_page(ui->content, NULL);
    panel_button(page, LV_SYMBOL_HOME "  Casa", C_OFF, show_overview, NULL);
    lv_obj_t *header = plain(page);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, b->round ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          b->round ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START);
    lv_obj_set_clickable(header, true);
    lv_obj_add_event_cb(header, open_settings, LV_EVENT_CLICKED, NULL);
    lv_port_set_focusable(header);
    lv_obj_t *brand = label(header, UI_FONT_SMALL, C_WARM, "CASA  " LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_letter_space(brand, 3, 0);
    ui->summary = label(header, ui_font_title, C_INK, "");
    lv_obj_set_width(ui->summary, LV_PCT(100));
    lv_label_set_long_mode(ui->summary, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(ui->summary, b->round ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    ui->status = label(header, UI_FONT_SMALL, C_MIST, "");

    int count = casa_entities(ui->list, CASA_ENTITIES);
    ui->tile_count = 0;
    if (count == 0)
    {
        ui_text(page, "Nessun preferito ancora. Sul PC, nella stessa rete, apri l'indirizzo che trovi in Impostazioni (tocca CASA in alto).", true);
        return;
    }
    lv_obj_t *modes = plain(page);
    lv_obj_set_size(modes, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(modes, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(modes, 6, 0);
    lv_obj_set_style_pad_row(modes, 6, 0);
    const char *labels[] = {"Stanze", "Tipi", "Dashboard", "Tutte"};
    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *button = ui_button(modes, labels[i], ui->group_mode == i ? C_WARM : C_OFF, mode_event, (void *)(intptr_t)i);
        lv_obj_set_width(button, LV_SIZE_CONTENT);
        lv_obj_set_height(button, 48);
        lv_obj_t *text = lv_obj_get_child(button, 0);
        lv_obj_set_style_text_font(text, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(text, ui_color(ui->group_mode == i ? C_BG : C_INK), 0);
    }
    panel_button(page, ui->show_unavailable ? "Tutti: inclusi offline" : "Disponibili  /  Mostra offline", C_OFF, show_offline, NULL);
    if (ui->group_mode == 3) { all_devices(ui, page); return; }
    if (!ui->group[0])
    {
        ui->group_count = 0;
        for (int i = 0; i < count; i++)
        {
            if (!ui->show_unavailable && (!ui->list[i].known || !ui->list[i].available)) continue;
            int groups = ui->group_mode == 2 && ui->list[i].dashboards[0][0] ? 4 : 1;
            for (int j = 0; j < groups; j++)
            {
                if (j && !ui->list[i].dashboards[j][0]) break;
                const char *name = entity_group(&ui->list[i], j);
                bool found = false;
                for (int k = 0; k < ui->group_count; k++)
                    if (!strcmp(ui->groups[k], name)) found = true;
                if (!found && ui->group_count < CASA_ENTITIES * 4)
                    snprintf(ui->groups[ui->group_count++], CASA_NAME, "%s", name);
            }
        }
        if (ui->page * 12 >= ui->group_count) ui->page = 0;
        if (!ui->group_count) ui_text(page, "Nessun dispositivo disponibile. Tocca Mostra offline per consultare gli stati ricevuti da HA.", true);
        for (int i = ui->page * 12; i < ui->group_count && i < (ui->page + 1) * 12; i++)
            ui_row(page, LV_SYMBOL_HOME, C_WARM, ui->groups[i], LV_SYMBOL_RIGHT, group_event, ui->groups[i]);
        if (ui->group_count > 12) pager(page, ui->group_count);
        return;
    }
    ui_row(page, LV_SYMBOL_LEFT, C_WARM, ui->group, "Tutti i gruppi", group_event, "");
    int inner = b->round ? w * 78 / 100 : w - 28;
    int tile_w = (inner - 10) / 2;
    lv_obj_t *grid = plain(page);
    lv_obj_set_size(grid, inner, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 10, 0);
    lv_obj_set_style_pad_row(grid, 10, 0);
    int matched = 0;
    int group_count = 0;
    for (int i = 0; i < count; i++)
        if (in_group(&ui->list[i]) && (ui->show_unavailable || (ui->list[i].known && ui->list[i].available))) group_count++;
    if (ui->page * 12 >= group_count) ui->page = 0;
    for (int i = 0; i < count; i++)
    {
        const casa_entity_t *entity = &ui->list[i];
        if (!in_group(entity) || (!ui->show_unavailable && (!entity->known || !entity->available))) continue;
        int index = matched++;
        if (index < ui->page * 12 || index >= (ui->page + 1) * 12) continue;
        tile_t *tile = &ui->tiles[ui->tile_count++];
        snprintf(tile->id, sizeof(tile->id), "%s", entity->id);
        make_tile(ui, grid, tile, tile_w);
        update_tile(tile, entity);
    }
    if (!matched) ui_text(page, "Nessun dispositivo disponibile. Mostra offline oppure modifica la selezione dal PC.", true);
    if (matched > 12) pager(page, matched);

}

static void home_update(casa_ui_t *ui)
{
    int count = casa_entities(ui->list, CASA_ENTITIES);
    char text[96];
    summary_text(ui, count, text, sizeof(text));
    if (ui->summary) lv_label_set_text(ui->summary, text);
    bool show_status = !ui->status_now.connected || strncmp(ui->status_now.message, "Home Assistant:", 15) == 0;
    if (ui->status)
    {
        lv_label_set_text(ui->status, show_status ? ui->status_now.message : "");
        lv_obj_set_hidden(ui->status, !show_status);
    }
    for (int t = 0; t < ui->tile_count; t++)
    {
        if (ui->tiles[t].unit) { update_unit(ui, &ui->tiles[t]); continue; }
        for (int i = 0; i < count; i++)
            if (strcmp(ui->tiles[t].id, ui->list[i].id) == 0)
            {
                update_tile(&ui->tiles[t], &ui->list[i]);
                break;
            }
    }
}

/* ------------------------------------------------------------ detail */

static int dial_range(const casa_entity_t *e, int32_t *min, int32_t *max)
{
    /* Climate works in tenths of a degree. */
    if (e->kind == CASA_CLIMATE)
    {
        *min = (int32_t)lroundf((isnan(e->min_temp) ? 7 : e->min_temp) * 10);
        *max = (int32_t)lroundf((isnan(e->max_temp) ? 35 : e->max_temp) * 10);
        return isnan(e->target_temp) ? *min : (int)lroundf(e->target_temp * 10);
    }
    *min = e->kind == CASA_LIGHT ? 1 : 0;
    *max = 100;
    return e->position >= 0 ? e->position : e->kind == CASA_LIGHT ? (e->on && e->brightness >= 0 ? e->brightness : 0) : 0;
}

static void dial_text(const casa_entity_t *e, int32_t value, char *out, size_t size)
{
    if (e->kind == CASA_CLIMATE)
        snprintf(out, size, "%ld,%ld\xC2\xB0", (long)(value / 10), (long)(value % 10));
    else
        snprintf(out, size, "%ld%%", (long)value);
}

/* Send the dial value (debounced: while the finger moves only the label follows). */
static void send_dial(casa_ui_t *ui)
{
    casa_entity_t e;
    if (ui->pending == INT32_MIN || !casa_entity(ui->selected, &e))
        return;
    char data[64], domain[24];
    snprintf(domain, sizeof(domain), "%.*s", (int)strcspn(e.id, "."), e.id);
    int32_t v = ui->pending;
    ui->pending = INT32_MIN;
    switch (e.kind)
    {
    case CASA_LIGHT:
        snprintf(data, sizeof(data), "{\"brightness_pct\":%ld}", (long)v);
        casa_call(domain, "turn_on", e.id, data);
        break;
    case CASA_COVER:
        snprintf(data, sizeof(data), "{\"position\":%ld}", (long)v);
        casa_call(domain, "set_cover_position", e.id, data);
        break;
    case CASA_FAN:
        snprintf(data, sizeof(data), "{\"percentage\":%ld}", (long)v);
        casa_call(domain, "set_percentage", e.id, data);
        break;
    case CASA_MEDIA:
        snprintf(data, sizeof(data), "{\"volume_level\":%.2f}", v / 100.0);
        casa_call(domain, "volume_set", e.id, data);
        break;
    case CASA_CLIMATE:
        snprintf(data, sizeof(data), "{\"temperature\":%.1f}", v / 10.0);
        casa_call(domain, "set_temperature", e.id, data);
        break;
    default:
        break;
    }
}

static void dial_event(lv_event_t *e)
{
    casa_ui_t *ui = s_ui;
    casa_entity_t entity;
    if (!casa_entity(ui->selected, &entity))
        return;
    int32_t value = lv_arc_get_value(ui->dial);
    if (entity.kind == CASA_CLIMATE)
    {
        int32_t step = (int32_t)lroundf((isnan(entity.temp_step) || entity.temp_step <= 0 ? .5f : entity.temp_step) * 10);
        if (step < 1) step = 1;
        int32_t minimum = lv_arc_get_min_value(ui->dial);
        value = minimum + (int32_t)lroundf((float)(value - minimum) / step) * step;
        lv_arc_set_value(ui->dial, value);
        value = lv_arc_get_value(ui->dial);
    }
    char text[16];
    dial_text(&entity, value, text, sizeof(text));
    lv_label_set_text(ui->dial_value, text);
    ui->pending = value;
    ui->pending_at = now_ms() + (lv_event_get_code(e) == LV_EVENT_RELEASED ? 0 : 350);
}

static void knob_event(lv_event_t *e)
{
    lv_port_knob_t *k = lv_event_get_param(e);
    casa_ui_t *ui = s_ui;
    if (ui->view != DETAIL || ui->dial == NULL)
        return;
    casa_entity_t entity;
    if (!casa_entity(ui->selected, &entity))
        return;
    int32_t step = entity.kind == CASA_CLIMATE ? (int32_t)lroundf((isnan(entity.temp_step) || entity.temp_step <= 0 ? 0.5f : entity.temp_step) * 10) : 5;
    int32_t value = lv_arc_get_value(ui->dial) + k->delta * step;
    lv_arc_set_value(ui->dial, value);
    value = lv_arc_get_value(ui->dial);
    char text[16];
    dial_text(&entity, value, text, sizeof(text));
    lv_label_set_text(ui->dial_value, text);
    ui->pending = value;
    ui->pending_at = now_ms() + 400;
    k->handled = true;
}

static void detail_button(lv_event_t *e)
{
    casa_ui_t *ui = s_ui;
    casa_entity_t entity;
    if (!casa_entity(ui->selected, &entity))
        return;
    const char *what = lv_event_get_user_data(e);
    char domain[24];
    snprintf(domain, sizeof(domain), "%.*s", (int)strcspn(entity.id, "."), entity.id);
    if (strncmp(what, "rgb:", 4) == 0 || strncmp(what, "k:", 2) == 0)
    {
        char data[64];
        if (what[0] == 'k')
            snprintf(data, sizeof(data), "{\"color_temp_kelvin\":%s}", what + 2);
        else
            snprintf(data, sizeof(data), "{\"rgb_color\":[%s]}", what + 4);
        casa_call(domain, "turn_on", entity.id, data);
    }
    else if (strncmp(what, "hvac:", 5) == 0)
    {
        char data[48];
        snprintf(data, sizeof(data), "{\"hvac_mode\":\"%s\"}", what + 5);
        casa_call(domain, "set_hvac_mode", entity.id, data);
    }
    else if (strcmp(what, "tap") == 0)
        casa_tap(&entity);
    else
        casa_call(domain, what, entity.id, NULL);
}

static void lock_confirmed(bool confirmed, void *unused)
{
    (void)unused;
    casa_entity_t entity;
    if (confirmed && casa_entity(s_ui->selected, &entity))
        casa_tap(&entity);
}

static void lock_button(lv_event_t *e)
{
    (void)e;
    casa_entity_t entity;
    if (!casa_entity(s_ui->selected, &entity))
        return;
    if (strcmp(entity.state, "locked") == 0)
        ui_confirm("Aprire la serratura?", entity.name, "Apri", 0xFF6B6B, lock_confirmed, NULL);
    else
        casa_tap(&entity);
}

static lv_obj_t *button_row(lv_obj_t *page)
{
    lv_obj_t *row = plain(page);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_pad_row(row, 8, 0);
    return row;
}

static void swatch(lv_obj_t *row, uint32_t colour, const char *what)
{
    lv_obj_t *s = lv_obj_create(row);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, 38, 38);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s, ui_color(colour), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s, ui_color(0xFFFFFF), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s, 3, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s, detail_button, LV_EVENT_CLICKED, (void *)what);
    lv_port_set_focusable(s);
}

static void step_dial(lv_event_t *event)
{
    casa_entity_t entity;
    if (!s_ui->dial || !casa_entity(s_ui->selected, &entity) || !entity.available || !s_ui->status_now.connected) return;
    int32_t step = entity.kind == CASA_CLIMATE ? (int32_t)lroundf((isnan(entity.temp_step) || entity.temp_step <= 0 ? .5f : entity.temp_step) * 10) : 5;
    if (step < 1) step = 1;
    lv_arc_set_value(s_ui->dial, lv_arc_get_value(s_ui->dial) + step * (int)(intptr_t)lv_event_get_user_data(event));
    s_ui->pending = lv_arc_get_value(s_ui->dial);
    s_ui->pending_at = now_ms() + 350;
    char text[24];
    dial_text(&entity, s_ui->pending, text, sizeof(text));
    lv_label_set_text(s_ui->dial_value, text);
}

static void control_availability(lv_obj_t *object, bool enabled)
{
    if (lv_obj_is_clickable(object))
    {
        if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
        else lv_obj_add_state(object, LV_STATE_DISABLED);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) control_availability(lv_obj_get_child(object, i), enabled);
}

static void detail(casa_ui_t *ui)
{
    casa_entity_t e;
    lv_obj_t *page = ui_page(ui->content, NULL);
    if (!casa_entity(ui->selected, &e))
    {
        ui_title(page, "Non trovato");
        return;
    }
    lv_obj_t *area = label(page, UI_FONT_SMALL, C_WARM, e.area);
    lv_obj_set_style_text_letter_space(area, 2, 0);
    ui_title(page, e.name[0] ? e.name : e.id);
    char text[64];
    ui->d_state = label(page, UI_FONT_BODY, C_MIST, casa_state_text(&e, text, sizeof(text)));
    ui->dial = NULL;
    ui->pending = INT32_MIN;
    bool has_dial = (e.kind == CASA_LIGHT && e.supports_brightness) || (e.kind == CASA_COVER && e.position >= 0) ||
                    (e.kind == CASA_FAN && e.position >= 0) || e.kind == CASA_MEDIA || e.kind == CASA_CLIMATE;
    if (has_dial)
    {
        int32_t min, max, value = dial_range(&e, &min, &max);
        ui_text(page, e.kind == CASA_CLIMATE ? "Temperatura desiderata" : e.kind == CASA_LIGHT ? "Luminosita'" : "Regolazione", true);
        int side = board_info()->width < 400 ? 190 : 256;
        ui->dial = lv_arc_create(page);
        lv_obj_set_size(ui->dial, side, side);
        lv_arc_set_rotation(ui->dial, 135);
        lv_arc_set_bg_angles(ui->dial, 0, 270);
        lv_arc_set_range(ui->dial, min, max);
        lv_arc_set_value(ui->dial, value);
        lv_obj_set_style_arc_width(ui->dial, 16, LV_PART_MAIN);
        lv_obj_set_style_arc_width(ui->dial, 16, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(ui->dial, ui_color(0x232834), LV_PART_MAIN);
        lv_obj_set_style_arc_color(ui->dial, ui_color(active_colour(&e)), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(ui->dial, true, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(ui->dial, ui_color(0xFFFFFF), LV_PART_KNOB);
        lv_obj_set_style_pad_all(ui->dial, 4, LV_PART_KNOB);
        lv_obj_add_event_cb(ui->dial, dial_event, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(ui->dial, dial_event, LV_EVENT_RELEASED, NULL);
        ui->dial_value = label(ui->dial, ui_font_big, C_INK, "");
        lv_obj_center(ui->dial_value);
        dial_text(&e, value, text, sizeof(text));
        lv_label_set_text(ui->dial_value, text);
    }
    ui->controls = plain(page);
    lv_obj_set_size(ui->controls, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ui->controls, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui->controls, 12, 0);
    if (has_dial)
    {
        lv_obj_t *steps = button_row(ui->controls);
        lv_obj_t *minus = panel_button(steps, LV_SYMBOL_MINUS, C_OFF, step_dial, (void *)-1);
        lv_obj_t *plus = panel_button(steps, LV_SYMBOL_PLUS, C_OFF, step_dial, (void *)1);
        lv_obj_set_width(minus, LV_PCT(42));
        lv_obj_set_width(plus, LV_PCT(42));
    }
    lv_obj_t *row = button_row(ui->controls);
    switch (e.kind)
    {
    case CASA_LIGHT:
    case CASA_SWITCH:
    case CASA_FAN:
        ui->power_button = ui_button(row, e.on ? "Spegni" : "Accendi", e.on ? 0x2A2E38 : 0x3A6E9E, detail_button, (void *)"tap");
        if (e.kind == CASA_LIGHT && e.supports_temp)
        {
            lv_obj_t *swatches = button_row(ui->controls);
            swatch(swatches, 0xFFB04A, "k:2700");
            swatch(swatches, 0xFFE2B0, "k:4000");
            swatch(swatches, 0xE8F0FF, "k:6000");
        }
        if (e.kind == CASA_LIGHT && e.supports_color)
        {
            lv_obj_t *swatches = button_row(ui->controls);
            swatch(swatches, 0xFF3B30, "rgb:255,40,30");
            swatch(swatches, 0xFF9F0A, "rgb:255,140,0");
            swatch(swatches, 0x30D158, "rgb:40,210,90");
            swatch(swatches, 0x0A84FF, "rgb:10,120,255");
            swatch(swatches, 0xBF5AF2, "rgb:190,90,240");
            swatch(swatches, 0xFF375F, "rgb:255,55,95");
        }
        break;
    case CASA_COVER:
        ui_button(row, "Apri", 0x3A6E9E, detail_button, (void *)"open_cover");
        ui_button(row, "Stop", 0x2A2E38, detail_button, (void *)"stop_cover");
        ui_button(row, "Chiudi", 0x3A6E9E, detail_button, (void *)"close_cover");
        break;
    case CASA_MEDIA:
        ui_button(row, LV_SYMBOL_PREV, 0x2A2E38, detail_button, (void *)"media_previous_track");
        ui_button(row, e.on ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY, 0xA0457E, detail_button, (void *)"media_play_pause");
        ui_button(row, LV_SYMBOL_NEXT, 0x2A2E38, detail_button, (void *)"media_next_track");
        if (e.media_artist[0])
            ui_text(page, e.media_artist, true);
        break;
    case CASA_CLIMATE:
        for (int i = 0; i < e.hvac_count; i++)
        {
            static char modes[6][20];
            snprintf(modes[i], sizeof(modes[i]), "hvac:%s", e.hvac_modes[i]);
            static const struct
            {
                const char *id, *name;
            } names[] = {{"off", "Spento"}, {"heat", "Caldo"}, {"cool", "Freddo"}, {"auto", "Auto"}, {"heat_cool", "Auto"},
                         {"dry", "Deumidifica"}, {"fan_only", "Ventola"}};
            const char *name = e.hvac_modes[i];
            for (unsigned n = 0; n < sizeof(names) / sizeof(names[0]); n++)
                if (strcmp(names[n].id, e.hvac_modes[i]) == 0)
                    name = names[n].name;
            ui->hvac_buttons[i] = panel_button(row, name, strcmp(e.state, e.hvac_modes[i]) == 0 ? 0xC2593A : C_OFF, detail_button, modes[i]);
        }
        break;
    case CASA_SCENE:
        ui_button(row, "Avvia", 0x8C7A2E, detail_button, (void *)"tap");
        break;
    case CASA_LOCK:
        ui_button(row, strcmp(e.state, "locked") == 0 ? "Apri" : "Chiudi a chiave",
                  strcmp(e.state, "locked") == 0 ? 0xA0403A : 0x3A6E9E, lock_button, NULL);
        break;
    default:
        break;
    }
    ui->d_extra = ui_text(page, "", true);
}

static void detail_update(casa_ui_t *ui)
{
    casa_entity_t e;
    if (!casa_entity(ui->selected, &e) || ui->d_state == NULL)
        return;
    char text[64];
    lv_label_set_text(ui->d_state, casa_state_text(&e, text, sizeof(text)));
    bool enabled = ui->status_now.connected && e.known && e.available;
    if (ui->dial)
    {
        if (enabled) lv_obj_remove_state(ui->dial, LV_STATE_DISABLED);
        else lv_obj_add_state(ui->dial, LV_STATE_DISABLED);
    }
    if (ui->controls) control_availability(ui->controls, enabled);
    if (ui->power_button) lv_label_set_text(lv_obj_get_child(ui->power_button, 0), e.on ? "Spegni" : "Accendi");
    if (e.kind == CASA_CLIMATE)
        for (int i = 0; i < e.hvac_count; i++)
            if (ui->hvac_buttons[i]) lv_obj_set_style_bg_color(ui->hvac_buttons[i], ui_color(!strcmp(e.state, e.hvac_modes[i]) ? 0xC2593A : C_OFF), 0);
    /* Home Assistant's own time of the last change when the clock is set, else ours. */
    uint32_t ago = clock_is_valid() && e.last_changed > 0 ? (uint32_t)((time(NULL) - e.last_changed) / 60)
                                                          : (now_ms() - e.changed_ms) / 60000;
    if (!enabled)
        lv_label_set_text(ui->d_extra, !ui->status_now.connected ? "Home Assistant scollegato" : !e.known ? "In attesa del primo stato da HA" : "Questo stato arriva da Home Assistant. Verifica il dispositivo o l'integrazione in HA.");
    else if (e.known)
    {
        if (ago == 0)
            snprintf(text, sizeof(text), "Aggiornato ora");
        else if (ago < 120)
            snprintf(text, sizeof(text), "Aggiornato %lu min fa", (unsigned long)ago);
        else
            snprintf(text, sizeof(text), "Aggiornato %lu ore fa", (unsigned long)(ago / 60));
        lv_label_set_text(ui->d_extra, text);
    }
    /* Follow the real value unless the finger or the knob is moving it. */
    if (ui->dial && ui->pending == INT32_MIN && !lv_obj_has_state(ui->dial, LV_STATE_PRESSED))
    {
        int32_t min, max, value = dial_range(&e, &min, &max);
        lv_arc_set_value(ui->dial, value);
        dial_text(&e, value, text, sizeof(text));
        lv_label_set_text(ui->dial_value, text);
        lv_obj_set_style_arc_color(ui->dial, ui_color(active_colour(&e)), LV_PART_INDICATOR);
    }
}

/* ------------------------------------------------------------ settings */

static void settings_action(lv_event_t *e)
{
    if ((intptr_t)lv_event_get_user_data(e) == 1)
    {
        casa_reconnect();
        ui_toast("Ricollego");
    }
    else
    {
#if CONFIG_APPKIT_HOSTED
        s_ui->view = OVERVIEW;
        app_back();
#else
        extapp_return_to_launcher();
#endif
    }
}

static void settings(casa_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Impostazioni");
#if CONFIG_APPKIT_HOSTED
    appkit_dashboard_page(page, ui_color(C_WARM));
    ui_text(page, "Configura Home Assistant dal PC, anche con l'indirizzo remoto Nabu Casa. Tieni premuta l'ora sulla homepage per aprire Casa.", true);
#else
    ui_text(page, "Apri dal PC http://", true);
    ui_text(page, casa_web_address(), false);
    ui_text(page, casa_web_code(), false);
#endif
    ui->status = ui_text(page, ui->status_now.message, true);
    ui_button(page, "Ricollega", 0x2A2E38, settings_action, (void *)1);
    ui_button(page, "Esci", 0x2A2E38, settings_action, (void *)2);
}

/* ------------------------------------------------------------ frame */

static void render(casa_ui_t *ui)
{
    if (ui->content)
        lv_obj_delete(ui->content);
    ui->summary = ui->status = ui->dial = ui->dial_value = ui->d_state = ui->d_extra = NULL;
    ui->clock_label = ui->date_label = ui->controls = ui->power_button = NULL;
    memset(ui->hvac_buttons, 0, sizeof(ui->hvac_buttons));
    ui->clock_minute = -1;
    ui->tile_count = 0;
    ui->content = plain(ui->screen);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->content, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->content, LV_OPA_COVER, 0);
    lv_obj_move_to_index(ui->content, 0);
    casa_status(&ui->status_now);
    if (ui->view == OVERVIEW)
        overview(ui);
    else if (ui->view == HOME)
    {
        home(ui);
        home_update(ui);
    }
    else if (ui->view == DEVICE)
        device_page(ui);
    else if (ui->view == DETAIL)
    {
        detail(ui);
        detail_update(ui);
    }
    else
        settings(ui);
}

/* Rooms and ids in order: when this changes (new setup) the home is rebuilt. */
static void layout_signature(casa_ui_t *ui, char *out, size_t size)
{
    int count = casa_entities(ui->list, CASA_ENTITIES);
    uint32_t hash = 2166136261u;
    for (int i = 0; i < count; i++)
    {
        hash = (hash ^ (ui->list[i].known && ui->list[i].available)) * 16777619u;
        for (const char *p = ui->list[i].id; *p; p++)
            hash = (hash ^ (uint8_t)*p) * 16777619u;
        for (const char *p = ui->list[i].device_id; *p; p++) hash = (hash ^ (uint8_t)*p) * 16777619u;
        for (const char *p = ui->list[i].device_name; *p; p++) hash = (hash ^ (uint8_t)*p) * 16777619u;
        for (const char *p = ui->list[i].area; *p; p++)
            hash = (hash ^ (uint8_t)*p) * 16777619u;
        for (const char *p = ui->list[i].device_class; *p; p++) hash = (hash ^ (uint8_t)*p) * 16777619u;
        for (int group = 0; group < 4; group++)
            for (const char *p = ui->list[i].dashboards[group]; *p; p++) hash = (hash ^ (uint8_t)*p) * 16777619u;
    }
    snprintf(out, size, "%d:%08lx", count, (unsigned long)hash);
}

static void tick(lv_timer_t *timer)
{
    casa_ui_t *ui = lv_timer_get_user_data(timer);
    if (ui->view == DETAIL && ui->pending != INT32_MIN && (int32_t)(now_ms() - ui->pending_at) >= 0 &&
        !lv_obj_has_state(ui->dial, LV_STATE_PRESSED))
        send_dial(ui);
    casa_status(&ui->status_now);
    if (ui->status_now.revision == ui->revision && (ui->view != OVERVIEW || ui->clock_minute == (int)((time(NULL) / 60) % 60)))
        return;
    if (power_screen() == POWER_SCREEN_OFF || lv_screen_active() != ui->screen)
        return;
    ui->revision = ui->status_now.revision;
    if (ui->view == HOME || ui->view == OVERVIEW || ui->view == DEVICE)
    {
        char layout[32];
        layout_signature(ui, layout, sizeof(layout));
        if (strcmp(layout, ui->layout) != 0)
        {
            snprintf(ui->layout, sizeof(ui->layout), "%s", layout);
            render(ui);
        }
        else if (ui->view == OVERVIEW)
            overview_update(ui);
        else
            home_update(ui);
    }
    else if (ui->view == DETAIL)
        detail_update(ui);
    else if (ui->status)
        lv_label_set_text(ui->status, ui->status_now.message);
}

bool casa_ui_back(void)
{
    casa_ui_t *ui = s_ui;
    if (ui == NULL) return false;
    if (ui->view == OVERVIEW)
    {
#if CONFIG_APPKIT_HOSTED
        return false;
#else
        extapp_return_to_launcher();
        return true;
#endif
    }
    if (ui->view == DETAIL)
    {
        send_dial(ui);
        ui->view = ui->detail_parent;
    }
    else if (ui->view == DEVICE)
    {
        ui->view = HOME; ui->group_mode = 3; ui->page = 0;
    }
    else if (ui->view == HOME && ui->group[0] && ui->group_mode != 3)
    {
        ui->group[0] = 0; ui->page = 0;
    }
    else ui->view = OVERVIEW;
    render(ui);
    return true;
}

void casa_ui_destroy(void)
{
    if (s_ui == NULL) return;
    if (s_ui->timer) lv_timer_delete(s_ui->timer);
    free(s_ui->list);
    free(s_ui);
    s_ui = NULL;
}

void casa_ui_create(lv_obj_t *screen)
{
    casa_ui_t *ui = heap_caps_calloc(1, sizeof(*ui), MALLOC_CAP_SPIRAM);
    if (ui != NULL) ui->list = heap_caps_calloc(CASA_ENTITIES, sizeof(casa_entity_t), MALLOC_CAP_SPIRAM);
    if (ui == NULL || ui->list == NULL)
    {
        free(ui);
        ui_text(ui_page(screen, "Casa"), "Memoria insufficiente", true);
        return;
    }
    s_ui = ui;
    ui->screen = screen;
    ui->view = OVERVIEW;
    ui->pending = INT32_MIN;
    lv_obj_set_style_bg_color(screen, ui_color(C_BG), 0);
    lv_obj_add_event_cb(screen, knob_event, lv_port_knob_event(), NULL);
    layout_signature(ui, ui->layout, sizeof(ui->layout));
    render(ui);
    ui->timer = lv_timer_create(tick, 120, ui);
}

#if CONFIG_WATCH_DEV_CONSOLE && !CONFIG_APPKIT_HOSTED
/* QA only: "view home|settings", "detail <entity_id>". */
#include "core/devcon.h"

bool devcon_app_command(char *line)
{
    if (s_ui == NULL)
        return false;
    bool handled = true;
    lv_port_lock();
    if (strcmp(line, "view home") == 0)
        s_ui->view = HOME;
    else if (strcmp(line, "view settings") == 0)
        s_ui->view = SETTINGS;
    else if (strncmp(line, "tap ", 4) == 0)
    {
        casa_entity_t entity;
        handled = casa_entity(line + 4, &entity) && casa_tap(&entity);
        lv_port_unlock();
        printf(handled ? "@OK %s\n" : "@ERR %s\n", line);
        return true;
    }
    else if (strncmp(line, "detail ", 7) == 0)
    {
        snprintf(s_ui->selected, sizeof(s_ui->selected), "%s", line + 7);
        s_ui->view = DETAIL;
    }
    else
        handled = false;
    if (handled)
        render(s_ui);
    lv_port_unlock();
    if (handled)
        printf("@OK %s\n", line);
    return handled;
}
#endif
