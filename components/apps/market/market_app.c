#include "apps_internal.h"
#include "core/lv_port.h"
#include "core/power.h"
#if CONFIG_WATCH_ECOS_PAIRING
#include "services/ecos.h"
#endif
#include "services/market.h"
#include "services/fw_version.h"
#include "hardware/board.h"
#include "hardware/sdcard.h"
#include "ui/ui.h"
#include "link/link_internal.h"
#include "orbi/orbi_genome.h"
#include "cJSON.h"

#if CONFIG_WATCH_DEV_CONSOLE
#include "../pet/pet.h"
#include "orbi/pet_pokemon.h"
#include "core/sys.h"
#include "hardware/audio.h"
#include "services/sound.h"
#include <math.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct market_ui market_ui_t;
typedef struct
{
    market_ui_t *ui;
    char id[32];
} card_t;

struct market_ui
{
    lv_obj_t *screen;
    lv_obj_t *content;
    lv_obj_t *border;
    lv_obj_t *status;
    lv_obj_t *action;
    lv_obj_t *primary;
    bool open_after_download;
    lv_obj_t *orb;
    lv_obj_t *satellite[3];
    lv_timer_t *poll;
    lv_timer_t *animation;
    market_entry_t selected;
    card_t cards[MARKET_MAX_APPS];
    uint32_t revision;
    market_phase_t phase;
    bool last_busy;
    int page; /* 0 entrance, 1 catalog, 2 app cover, 3 information */
    int angle;
    uint32_t action_ready_at;
    char queue[MARKET_MAX_APPS][32];   /* "update all": one download at a time */
    int queue_count;
    int queue_next;
    lv_point_t swipe_start;
};

static void render(market_ui_t *ui);

static void finish_navigation_touch(void)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL) lv_indev_wait_release(indev);
}

static const char *symbol(const char *id)
{
    if (strcmp(id, "Remote") == 0) return LV_SYMBOL_KEYBOARD;
    if (strcmp(id, "WebRadio") == 0) return LV_SYMBOL_AUDIO;
    if (strcmp(id, "WatcherPhoto") == 0) return LV_SYMBOL_IMAGE;
    return LV_SYMBOL_HOME;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    return object;
}

static void animate(lv_timer_t *timer)
{
    market_ui_t *ui = lv_timer_get_user_data(timer);
    if (ui->page != 0 || lv_screen_active() != ui->screen || power_screen() >= POWER_SCREEN_AOD) return;
    ui->angle = (ui->angle + 2) % 360;
    int radius = board_info()->width / 3;
    for (int i = 0; i < 3; i++)
    {
        int angle = (ui->angle + i * 120) % 360;
        int x = (lv_trigo_cos(angle) * radius) / 32768;
        int y = (lv_trigo_sin(angle) * radius) / 32768;
        lv_obj_align(ui->satellite[i], LV_ALIGN_CENTER, x, y - 30);
    }
}

static void enter(lv_event_t *event)
{
    market_ui_t *ui = lv_event_get_user_data(event);
    finish_navigation_touch();
    ui->page = 1;
    lv_timer_pause(ui->animation);
    if (market_count() == 0 && !market_busy()) market_refresh();
    lv_timer_resume(ui->poll);
    render(ui);
}

/* Slow drags must work too: LVGL gestures require a minimum velocity. */
static void scene_touch(lv_event_t *event)
{
    market_ui_t *ui = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (lv_event_get_code(event) == LV_EVENT_PRESSED)
    {
        ui->swipe_start = point;
        return;
    }
    int dy = ui->swipe_start.y - point.y;
    int dx = abs(point.x - ui->swipe_start.x);
    if (dy < 50 || dx > dy / 2 || (ui->page != 0 && ui->page != 2)) return;
    finish_navigation_touch();
    if (ui->page == 0) enter(event);
    else
    {
        ui->page = 3;
        render(ui);
    }
}

static void hero_gesture(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL && lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) enter(event);
}

static void refresh(lv_event_t *event)
{
    market_ui_t *ui = lv_event_get_user_data(event);
    esp_err_t err = market_refresh();
    if (err != ESP_OK) ui_toast("Un download è già in corso");
    else lv_timer_resume(ui->poll);
}

static void cycle_view(lv_event_t *event)
{
    market_ui_t *ui = lv_event_get_user_data(event);
    market_set_view((market_view() + 1) % 3);
    render(ui);
}

static void cycle_channel(lv_event_t *event)
{
    market_ui_t *ui = lv_event_get_user_data(event);
    market_set_development(!market_development());
    render(ui);
}

static void details(lv_event_t *event)
{
    card_t *card = lv_event_get_user_data(event);
    market_ui_t *ui = card->ui;
    for (int i = 0; market_entry(i, &ui->selected); i++)
    {
        if (strcmp(ui->selected.id, card->id) == 0)
        {
            finish_navigation_touch();
            ui->page = 2;
            render(ui);
            return;
        }
    }
    ui_toast("Catalogo aggiornato. Riapri l'app.");
}

static void launch_selected(market_ui_t *ui)
{
    if (market_busy())
    {
        ui_toast("Attendi la fine del download");
        return;
    }
    extapp_t *app = malloc(sizeof(*app));
    if (app == NULL)
    {
        ui_toast("Memoria insufficiente");
        return;
    }
    *app = ui->selected.local;
    if (!app_open("extapps.open", app)) free(app);
}

static void install_confirmed(bool confirmed, void *arg)
{
    char *id = arg;
    market_ui_t *ui = app_current() != NULL && strcmp(app_current()->id, "market") == 0
                          ? lv_obj_get_user_data(lv_screen_active()) : NULL;
    if (confirmed)
    {
        if (market_install(id) != ESP_OK)
        {
            ui_toast(sdcard_is_mounted() ? "Serve spazio libero o un download è già in corso" : "Installazione non avviata");
            if (ui != NULL) ui->open_after_download = false;
        }
        else if (ui != NULL && ui->poll != NULL)
        {
            ui_border_progress_set(ui->border, 0, 100);
            lv_timer_resume(ui->poll);
        }
    }
    else if (ui != NULL)
    {
        ui->open_after_download = false;
    }
    free(id);
}

static void download(lv_event_t *event)
{
    market_ui_t *ui = lv_event_get_user_data(event);
    /* Only a fresh interaction with the dedicated action may start a job. */
    if (ui->page < 2 || (lv_event_get_current_target(event) != ui->action &&
                         lv_event_get_current_target(event) != ui->primary) ||
        lv_tick_elaps(ui->action_ready_at) < 180) return;
    ui->open_after_download = lv_event_get_current_target(event) == ui->primary;
    if (market_busy())
    {
        ui->open_after_download = false;
        market_cancel();
        return;
    }
    /* Every download asks first: what, which version, how big. */
    char *id = strdup(ui->selected.id);
    if (id == NULL) return;
    static char title[48], text[120];
    bool downgrade = ui->selected.installed && ui->selected.installed_version[0] != '\0' &&
                     fw_version_compare(ui->selected.version, ui->selected.installed_version) < 0;
    bool update = ui->selected.installed && !downgrade;
    snprintf(title, sizeof(title), "%s %s?", downgrade ? "Tornare a" : update ? "Aggiornare" : "Installare", ui->selected.name);
    snprintf(text, sizeof(text), "Versione %s, %u KB. Il download richiede il Wi-Fi.", ui->selected.version,
             (unsigned)((ui->selected.binary_size + 1023) / 1024));
    ui_confirm(title, text, downgrade ? "Installa" : update ? "Aggiorna" : "Installa",
               downgrade ? UI_COLOR_ORANGE : UI_COLOR_GREEN, install_confirmed, id);
}

static void install_and_open(lv_event_t *event)
{
    market_ui_t *ui = lv_event_get_user_data(event);
    /* An unknown legacy version still means Open, matching the button label. */
    if (ui->selected.installed &&
        (ui->selected.installed_version[0] == '\0' ||
         fw_version_compare(ui->selected.version, ui->selected.installed_version) <= 0))
    {
        if (lv_tick_elaps(ui->action_ready_at) >= 180) launch_selected(ui);
        return;
    }
    download(event);
}

static void removed_confirmed(bool confirmed, void *arg)
{
    char *id = arg;
    if (confirmed)
    {
        esp_err_t err = market_remove(id);
        if (err != ESP_OK) ui_toast("App occupata o SD non disponibile");
        else if (app_current() != NULL && strcmp(app_current()->id, "market") == 0)
        {
            market_ui_t *ui = lv_obj_get_user_data(lv_screen_active());
            if (ui != NULL && ui->poll != NULL)
            {
                ui->open_after_download = false;
                ui_border_progress_set(ui->border, 0, 100);
                lv_timer_resume(ui->poll);
            }
        }
    }
    free(id);
}

static void remove_app(lv_event_t *event)
{
    market_ui_t *ui = lv_event_get_user_data(event);
    if (market_busy())
    {
        ui_toast("Attendi la fine dell'operazione");
        return;
    }
    char *id = strdup(ui->selected.id);
    if (id == NULL) return;
    ui_confirm("Elimina app", "Rimuovere l'app dalla SD? Impostazioni e dati personali restano.",
               "Elimina", UI_COLOR_RED, removed_confirmed, id);
}

static void show_hero(market_ui_t *ui)
{
    lv_obj_t *scene = ui->content;
    lv_obj_set_style_bg_color(scene, lv_color_hex(0x080B1D), 0);
    lv_obj_set_style_bg_grad_color(scene, lv_color_hex(0x252052), 0);
    lv_obj_set_style_bg_grad_dir(scene, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scene, LV_OPA_COVER, 0);
    lv_obj_set_clickable(scene, true);
    lv_obj_add_event_cb(scene, enter, LV_EVENT_CLICKED, ui);
    lv_obj_set_gesture_bubble(scene, false);
    lv_obj_add_event_cb(scene, hero_gesture, LV_EVENT_GESTURE, ui);
    lv_obj_add_event_cb(scene, scene_touch, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(scene, scene_touch, LV_EVENT_PRESSING, ui);

    int diameter = board_info()->width * 58 / 100;
    ui->orb = lv_obj_create(scene);
    lv_obj_remove_style_all(ui->orb);
    lv_obj_set_size(ui->orb, diameter, diameter);
    lv_obj_set_style_radius(ui->orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ui->orb, lv_color_hex(0x756BFF), 0);
    lv_obj_set_style_bg_grad_color(ui->orb, lv_color_hex(0x141737), 0);
    lv_obj_set_style_bg_grad_dir(ui->orb, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(ui->orb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui->orb, 2, 0);
    lv_obj_set_style_border_color(ui->orb, lv_color_hex(0xB9B7FF), 0);
    lv_obj_set_style_shadow_width(ui->orb, 0, 0);
    lv_obj_set_style_shadow_color(ui->orb, lv_color_hex(0x695CFF), 0);
    lv_obj_set_style_shadow_opa(ui->orb, LV_OPA_30, 0);
    lv_obj_set_clickable(ui->orb, false);
    lv_obj_set_scrollable(ui->orb, false);
    lv_obj_align(ui->orb, LV_ALIGN_CENTER, 0, -30);
    lv_obj_t *icon = label(ui->orb, LV_SYMBOL_DOWNLOAD, &lv_font_montserrat_32, 0xFFFFFF);
    lv_obj_center(icon);
    for (int i = 0; i < 3; i++)
    {
        ui->satellite[i] = lv_obj_create(scene);
        lv_obj_remove_style_all(ui->satellite[i]);
        lv_obj_set_size(ui->satellite[i], 10 + i * 3, 10 + i * 3);
        lv_obj_set_style_radius(ui->satellite[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ui->satellite[i], lv_color_hex(i == 0 ? 0x5EE7D9 : 0xCEC7FF), 0);
        lv_obj_set_style_bg_opa(ui->satellite[i], LV_OPA_COVER, 0);
        lv_obj_set_clickable(ui->satellite[i], false);
    }
    lv_obj_t *title = label(scene, "MARKET", ui_font_title, 0xFFFFFF);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, diameter / 2 + 5);
    lv_obj_t *hint = label(scene, "Tocca o scorri verso l'alto", UI_FONT_SMALL, 0xB8B8D5);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, diameter / 2 + 24);
    lv_timer_resume(ui->animation);
    animate(ui->animation);
}

/* Start the next queued update; false when the queue is done. */
static bool queue_step(market_ui_t *ui)
{
    while (ui->queue_next < ui->queue_count)
    {
        const char *id = ui->queue[ui->queue_next++];
        if (market_install(id) == ESP_OK)
        {
            char text[64];
            snprintf(text, sizeof(text), "Aggiorno %d di %d...", ui->queue_next, ui->queue_count);
            ui_toast(text);
            lv_timer_resume(ui->poll);
            return true;
        }
    }
    if (ui->queue_count > 0)
    {
        ui_toast(LV_SYMBOL_OK " Aggiornamenti completati");
        ui->queue_count = ui->queue_next = 0;
        launcher_refresh();
    }
    return false;
}

static void update_all_confirmed(bool confirmed, void *arg);

static void update_all(lv_event_t *event)
{
    market_ui_t *ui = lv_event_get_user_data(event);
    if (market_busy() || ui->queue_count > 0) return;
    ui_confirm("Aggiornare tutte?", "Le app con una versione nuova vengono scaricate una alla volta.", "Aggiorna",
               UI_COLOR_GREEN, update_all_confirmed, ui);
}

static void update_all_confirmed(bool confirmed, void *arg)
{
    market_ui_t *ui = arg;
    if (!confirmed || app_current() == NULL || strcmp(app_current()->id, "market") != 0 ||
        lv_obj_get_user_data(lv_screen_active()) != ui || market_busy() || ui->queue_count > 0) return;
    market_entry_t *entry = malloc(sizeof(*entry));
    if (entry == NULL) return;
    ui->queue_count = ui->queue_next = 0;
    for (int i = 0; market_entry(i, entry) && ui->queue_count < MARKET_MAX_APPS; i++)
    {
        if (entry->installed && entry->installed_version[0] != '\0' &&
            fw_version_compare(entry->version, entry->installed_version) > 0)
            snprintf(ui->queue[ui->queue_count++], sizeof(ui->queue[0]), "%s", entry->id);
    }
    free(entry);
    queue_step(ui);
}

static void show_catalog(market_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Esplora");
    lv_obj_t *toolbar = lv_obj_create(page);
    lv_obj_remove_style_all(toolbar);
    lv_obj_set_size(toolbar, LV_PCT(100), 42);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *channel = ui_button(toolbar, market_development() ? "Dev" : "Stabile", 0x282842, cycle_channel, ui);
    lv_obj_set_size(channel, 88, 38);
    lv_obj_set_style_text_font(lv_obj_get_child(channel, 0), UI_FONT_SMALL, 0);
    const char *views[] = {"Griglia", "Lista", "Schede"};
    lv_obj_t *view = ui_button(toolbar, views[market_view()], 0x282842, cycle_view, ui);
    lv_obj_set_size(view, 84, 38);
    lv_obj_set_style_text_font(lv_obj_get_child(view, 0), UI_FONT_SMALL, 0);
    lv_obj_t *reload = ui_button(toolbar, LV_SYMBOL_REFRESH, 0x282842, refresh, ui);
    lv_obj_set_size(reload, 40, 38);
    lv_obj_set_style_text_font(lv_obj_get_child(reload, 0), UI_FONT_SMALL, 0);

    ui->status = ui_text(page, "", true);
    market_status_t state;
    market_status(&state);
    lv_label_set_text(ui->status, state.message[0] != '\0' ? state.message : "App esterne dal tuo catalogo GitHub");
    if (!sdcard_is_mounted())
        ui_text(page, "Senza microSD c'è posto per una app alla volta: installarne un'altra la sostituisce.", true);
    if (market_development()) ui_text(page, "Il canale dev include versioni di prova.", true);

    int count = market_count();
    market_entry_t *entry = malloc(sizeof(*entry));
    if (entry == NULL) return;
    /* Three groups: what needs an update (with "update all"), what is new, what is installed. */
    static const char *const heads[] = {"DA AGGIORNARE", "DA INSTALLARE", "INSTALLATE"};
    int updates = 0;
    for (int i = 0; i < count && i < MARKET_MAX_APPS; i++)
    {
        if (market_entry(i, entry) && entry->installed && entry->installed_version[0] != '\0' &&
            fw_version_compare(entry->version, entry->installed_version) > 0)
        {
            updates++;
        }
    }
    for (int group = 0; group < 3; group++)
    {
        lv_obj_t *grid = NULL;
        bool headed = false;
        for (int i = 0; i < count && i < MARKET_MAX_APPS; i++)
        {
            if (!market_entry(i, entry)) continue;
            bool update = entry->installed && entry->installed_version[0] != '\0' &&
                          fw_version_compare(entry->version, entry->installed_version) > 0;
            int kind = update ? 0 : entry->installed ? 2 : 1;
            if (kind != group) continue;
            if (!headed)
            {
                headed = true;
                ui_section(page, heads[group]);
                if (group == 0 && updates > 1)
                {
                    char text[40];
                    snprintf(text, sizeof(text), LV_SYMBOL_DOWNLOAD "  Aggiorna tutte (%d)", updates);
                    lv_obj_t *all = ui_button(page, text, UI_COLOR_ORANGE, update_all, ui);
                    lv_port_set_focusable(all);
                }
                if (market_view() == 0)
                {
                    grid = lv_obj_create(page);
                    lv_obj_remove_style_all(grid);
                    lv_obj_set_width(grid, LV_PCT(100));
                    lv_obj_set_height(grid, LV_SIZE_CONTENT);
                    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
                    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
                    lv_obj_set_style_pad_row(grid, 10, 0);
                }
            }
            ui->cards[i].ui = ui;
            snprintf(ui->cards[i].id, sizeof(ui->cards[i].id), "%s", entry->id);
            const char *icon = entry->installed && entry->local.icon[0] != '\0' ? entry->local.icon : symbol(entry->id);
            char version[80];
            if (update)
                snprintf(version, sizeof(version), "%s " LV_SYMBOL_RIGHT " %s", entry->installed_version, entry->version);
            else
                snprintf(version, sizeof(version), "%s%s", entry->version, entry->installed ? " · installata" : "");
            if (market_view() == 1)
            {
                lv_obj_t *row = ui_row(page, icon, entry->color, entry->name, version, details, &ui->cards[i]);
                if (update) lv_obj_set_style_text_color(ui_row_value(row), lv_color_hex(UI_COLOR_ORANGE), 0);
                continue;
            }
            lv_obj_t *card = lv_obj_create(grid != NULL ? grid : page);
            ui_make_card(card);
            lv_obj_set_clickable(card, true);
            lv_port_set_focusable(card);
            lv_obj_set_width(card, grid != NULL ? LV_PCT(48) : LV_PCT(100));
            lv_obj_set_height(card, grid != NULL ? 154 : 255);
            lv_obj_set_scrollable(card, false);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_all(card, 8, 0);
            lv_obj_set_style_pad_row(card, 8, 0);
            if (update)
            {
                lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_ORANGE), 0);
                lv_obj_set_style_border_width(card, 2, 0);
            }
            ui_icon_bubble(card, icon, entry->color, grid != NULL ? 62 : 96);
            lv_obj_t *name = label(card, entry->name, grid != NULL ? UI_FONT_BODY : ui_font_headline, 0xFFFFFF);
            lv_obj_set_width(name, LV_PCT(100));
            lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
            label(card, version, UI_FONT_SMALL, update ? UI_COLOR_ORANGE : 0xB3B3CA);
            if (grid == NULL)
            {
                lv_obj_t *summary = label(card, entry->summary, UI_FONT_SMALL, 0xD0CFE0);
                lv_obj_set_width(summary, LV_PCT(90));
                lv_label_set_long_mode(summary, LV_LABEL_LONG_MODE_WRAP);
            }
            lv_obj_add_event_cb(card, details, LV_EVENT_CLICKED, &ui->cards[i]);
        }
    }
    free(entry);
    if (count == 0 && !market_busy()) ui_text(page, "Aggiorna il catalogo per scoprire le app.", true);
}

static void detail_gesture(lv_event_t *event)
{
    market_ui_t *ui = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP)
    {
        finish_navigation_touch();
        ui->page = 3;
        render(ui);
    }
}

static void show_information(market_ui_t *ui)
{
    market_entry_t *entry = &ui->selected;
    lv_obj_t *page = ui_page(ui->content, entry->name);
    ui_text(page, entry->summary, false);
    ui_section(page, "Questa versione");
    char text[96];
    snprintf(text, sizeof(text), "%s · %s · %.1f MB", entry->version, entry->channel,
             (double)entry->binary_size / (1024 * 1024));
    ui_text(page, text, true);
    if (entry->installed)
    {
        snprintf(text, sizeof(text), "Installata: %s",
                 entry->installed_version[0] ? entry->installed_version : "versione non indicata");
        ui_text(page, text, true);
    }
    ui_text(page, sdcard_is_mounted() ? "L'app funziona dal suo firmware. Il download viene salvato sulla microSD." :
                                        "L'app funziona dal suo firmware, scritto direttamente nella memoria app. "
                                        "Sostituisce l'app installata prima.", true);
    if (strcmp(entry->channel, "dev") == 0)
        ui_text(page, "Canale sviluppo: questa versione è ancora in verifica.", true);
    ui->status = ui_text(page, "", true);
    bool update = entry->installed && entry->installed_version[0] != '\0' &&
                  fw_version_compare(entry->version, entry->installed_version) > 0;
    ui->primary = ui_button(page, entry->installed ? (update ? "Aggiorna e apri" : "Apri") : "Installa e apri",
                            entry->color, install_and_open, ui);
    if (market_busy()) lv_obj_add_state(ui->primary, LV_STATE_DISABLED);
    ui->action = ui_button(page, market_busy() ? "Annulla download" : "Scarica soltanto",
                           0x303342, download, ui);
    if (entry->installed) ui_button(page, LV_SYMBOL_TRASH "  Elimina app", UI_COLOR_RED, remove_app, ui);
    lv_timer_resume(ui->poll);
}

static void show_details(market_ui_t *ui)
{
    market_entry_t *entry = &ui->selected;
    lv_obj_t *scene = ui->content;
    int width = board_info()->width;
    lv_obj_set_style_bg_color(scene, lv_color_mix(lv_color_hex(entry->color), lv_color_hex(0x050710), 20), 0);
    lv_obj_set_style_bg_grad_color(scene, lv_color_mix(lv_color_hex(entry->color), lv_color_hex(0x050710), 65), 0);
    lv_obj_set_style_bg_grad_dir(scene, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scene, LV_OPA_COVER, 0);
    lv_obj_set_clickable(scene, true);
    lv_obj_set_gesture_bubble(scene, false);
    lv_obj_add_event_cb(scene, detail_gesture, LV_EVENT_GESTURE, ui);
    lv_obj_add_event_cb(scene, scene_touch, LV_EVENT_PRESSED, ui);
    lv_obj_add_event_cb(scene, scene_touch, LV_EVENT_PRESSING, ui);
    lv_obj_t *eyebrow = label(scene, entry->installed ? "NEL TUO SPAZIO" : "DA SCOPRIRE", UI_FONT_SMALL, 0xB9BDCB);
    lv_obj_align(eyebrow, LV_ALIGN_CENTER, 0, -width * 37 / 100);
    const char *icon = entry->installed && entry->local.icon[0] != '\0' ? entry->local.icon : symbol(entry->id);
    lv_obj_t *bubble = ui_icon_bubble(scene, icon, entry->color, width * 38 / 100);
    lv_obj_align(bubble, LV_ALIGN_CENTER, 0, -width * 17 / 100);
    lv_obj_set_style_shadow_color(bubble, lv_color_hex(entry->color), 0);
    lv_obj_set_style_shadow_width(bubble, 0, 0);
    lv_obj_set_style_shadow_opa(bubble, LV_OPA_20, 0);
    lv_obj_t *title = label(scene, entry->name, ui_font_headline, 0xFFFFFF);
    lv_obj_set_width(title, LV_PCT(76));
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, width * 9 / 100);
    ui->status = label(scene, entry->version, UI_FONT_SMALL, 0xCDD0DE);
    lv_obj_set_width(ui->status, LV_PCT(74));
    lv_label_set_long_mode(ui->status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(ui->status, LV_ALIGN_CENTER, 0, width * 18 / 100);
    lv_obj_t *dock = lv_obj_create(scene);
    lv_obj_remove_style_all(dock);
    lv_obj_set_size(dock, width * 70 / 100, width >= 300 ? 46 : 36);
    lv_obj_align(dock, LV_ALIGN_CENTER, 0, width * (board_info()->round ? 29 : 32) / 100);
    lv_obj_set_scrollable(dock, false);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    bool update = entry->installed && entry->installed_version[0] != '\0' &&
                  fw_version_compare(entry->version, entry->installed_version) > 0;
    ui->primary = ui_button(dock, entry->installed ? (update ? "Aggiorna" : "Apri") : "Installa",
                            entry->color, install_and_open, ui);
    lv_obj_set_size(ui->primary, LV_PCT(48), LV_PCT(100));
    lv_obj_set_style_text_font(lv_obj_get_child(ui->primary, 0), UI_FONT_SMALL, 0);
    if (market_busy()) lv_obj_add_state(ui->primary, LV_STATE_DISABLED);
    ui->action = ui_button(dock, market_busy() ? "Annulla" : "Scarica",
                           0x303342, download, ui);
    lv_obj_set_size(ui->action, LV_PCT(48), LV_PCT(100));
    lv_obj_set_style_text_font(lv_obj_get_child(ui->action, 0), UI_FONT_SMALL, 0);
    lv_obj_t *hint = label(scene, "Scorri in alto · dettagli", UI_FONT_SMALL, 0x939BAD);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, width * (board_info()->round ? -47 : 43) / 100);
    if (board_info()->round) lv_obj_set_hidden(hint, true);
    ui_border_progress_color(ui->border, entry->color);
    if (ui->border != NULL) lv_obj_set_hidden(ui->border, false);
    if (!market_busy()) ui_border_progress_set(ui->border, 100, 100);
    lv_timer_resume(ui->poll);
}

static void render(market_ui_t *ui)
{
#if CONFIG_WATCH_DEV_CONSOLE
    printf("@MARKETPAGE %d\n", ui->page);
#endif
    /* Only the content is rebuilt; the per-screen timer and border survive. */
    if (ui->content != NULL) lv_obj_delete(ui->content);
    ui->content = lv_obj_create(ui->screen);
    lv_obj_remove_style_all(ui->content);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(ui->content, false);
    lv_obj_move_to_index(ui->content, 0);
    ui->action_ready_at = lv_tick_get();
    ui->status = NULL;
    ui->action = NULL;
    ui->primary = NULL;
    if (ui->page == 0) show_hero(ui);
    else if (ui->page == 1) show_catalog(ui);
    else if (ui->page == 2) show_details(ui);
    else show_information(ui);
    if (ui->page == 0) lv_timer_resume(ui->animation);
    else lv_timer_pause(ui->animation);
    market_status_t state;
    market_status(&state);
    ui->revision = state.revision;
    if (ui->border != NULL)
        lv_obj_set_hidden(ui->border, ui->page == 0 || (ui->page == 1 && !state.busy));
}

static void poll(lv_timer_t *timer)
{
    market_ui_t *ui = lv_timer_get_user_data(timer);
    if (lv_screen_active() != ui->screen) return;
    market_status_t state;
    market_status(&state);
    if (state.revision != ui->revision)
    {
        if (ui->page >= 2)
        {
            market_entry_t *entry = malloc(sizeof(*entry));
            if (entry != NULL)
            {
                for (int i = 0; market_entry(i, entry); i++)
                {
                    if (strcmp(entry->id, ui->selected.id) == 0)
                    {
                        ui->selected = *entry;
                        break;
                    }
                }
                free(entry);
            }
        }
        if (ui->page != 0) render(ui);
        ui->revision = state.revision;
    }
    if ((state.phase != ui->phase || state.busy != ui->last_busy) && !state.busy && ui->page != 0)
    {
        render(ui);
        if (state.phase == MARKET_DONE) launcher_refresh();
    }
    if (!state.busy && ui->open_after_download)
    {
        ui->open_after_download = false;
        if (state.phase == MARKET_DONE && ui->selected.installed &&
            strcmp(state.app_id, ui->selected.id) == 0)
        {
            launch_selected(ui);
            return;
        }
    }
    ui->phase = state.phase;
    ui->last_busy = state.busy;
    if (ui->primary != NULL && state.busy) lv_obj_add_state(ui->primary, LV_STATE_DISABLED);
    if (ui->border != NULL) lv_obj_set_hidden(ui->border, ui->page < 2 && (!state.busy || ui->page == 0));
    if (state.busy) ui_border_progress_set(ui->border, state.percent, 100);
    if (ui->status != NULL)
    {
        if (ui->page == 2)
        {
            char caption[64];
            if (state.busy) snprintf(caption, sizeof(caption), "%d%% · %s", state.percent,
                                    state.phase == MARKET_PUBLISHING ? "Salvataggio" : "Download");
            else snprintf(caption, sizeof(caption), "%s%s", ui->selected.version,
                          ui->selected.installed ? " · nella libreria" : "");
            if (strcmp(lv_label_get_text(ui->status), caption)) lv_label_set_text(ui->status, caption);
        }
        else if (state.busy || state.phase == MARKET_DONE || state.phase == MARKET_FAILED || state.phase == MARKET_CANCELLED)
            if (strcmp(lv_label_get_text(ui->status), state.message)) lv_label_set_text(ui->status, state.message);
    }
    if (ui->action != NULL && state.busy)
    {
        lv_obj_t *text = lv_obj_get_child(ui->action, 0);
        const char *caption = state.phase == MARKET_PUBLISHING ? "Salvo..." : "Annulla";
        if (strcmp(lv_label_get_text(text), caption)) lv_label_set_text(text, caption);
    }
    if (!state.busy && ui->queue_next < ui->queue_count && queue_step(ui)) return;
    if (!state.busy && ui->queue_count > 0 && ui->queue_next >= ui->queue_count) queue_step(ui);
    if (!state.busy) lv_timer_pause(ui->poll);
}

static void deleted(lv_event_t *event)
{
    market_ui_t *ui = lv_event_get_user_data(event);
    if (ui->poll != NULL) lv_timer_delete(ui->poll);
    if (ui->animation != NULL) lv_timer_delete(ui->animation);
    free(ui);
}

static bool back(void)
{
    market_ui_t *ui = lv_obj_get_user_data(lv_screen_active());
    if (ui == NULL || ui->page <= 1) return false;
    ui->page--;
    render(ui);
    return true;
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    market_ui_t *ui = calloc(1, sizeof(*ui));
    if (ui == NULL)
    {
        ui_toast("Memoria insufficiente");
        return;
    }
    ui->screen = screen;
    ui->page = 1; /* Go straight to useful content, without an animated entrance. */
    lv_obj_set_user_data(screen, ui);
    lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, ui);
    ui->content = lv_obj_create(screen);
    ui->border = ui_border_progress_create(screen, 0x8C7EFF);
    if (ui->border != NULL) lv_obj_set_hidden(ui->border, true);
    ui->poll = lv_timer_create(poll, 300, ui);
    ui->animation = lv_timer_create(animate, 60, ui);
    if (ui->poll == NULL || ui->animation == NULL)
    {
        if (ui->poll != NULL) lv_timer_pause(ui->poll);
        if (ui->animation != NULL) lv_timer_pause(ui->animation);
        ui_text(ui->content, "Memoria insufficiente", true);
        return;
    }
    lv_timer_pause(ui->poll);
    if (market_count() == 0 && !market_busy()) market_refresh();
    render(ui);
    if (market_busy()) lv_timer_resume(ui->poll);
}

const app_t market_app = {
    .id = "market", .name = "Market", .icon = LV_SYMBOL_DOWNLOAD,
    .color = 0x8C7EFF, .create = create, .back = back,
};

#if CONFIG_WATCH_DEV_CONSOLE
static void tone_task(void *arg)
{
    (void)arg;
    esp_err_t err = audio_start(22050, 1);
    if (err == ESP_OK) err = audio_set_volume(90);
    int16_t *samples = malloc(441 * sizeof(int16_t));
    for (int block = 0; err == ESP_OK && samples != NULL && block < 150; block++)
    {
        for (int i = 0; i < 441; i++)
            samples[i] = (int16_t)(14000.0f * sinf(2.0f * 3.14159265f * 880.0f * (block * 441 + i) / 22050.0f));
        err = audio_write(samples, 441);
    }
    free(samples);
    audio_stop();
    printf("@TONE %s\n", esp_err_to_name(err));
    vTaskDelete(NULL);
}

/* Keep application QA commands above the core/services dependency boundary. */
bool devcon_app_command(char *line)
{
    if (strncmp(line, "pet", 3) == 0 && (line[3] == 0 || line[3] == ' '))
    {
        pet_record_t p;
        uint64_t seed;
        if (!pet_current(&p) || !orbi_parse(p.seed, &seed))
        {
            printf("@ERR pet unavailable\n");
            return true;
        }
        if (strncmp(line, "pet appearance ", 15) == 0)
        {
            unsigned dex, shiny;
            if (sscanf(line + 15, "%u %u", &dex, &shiny) != 2 || dex > 151 || shiny > 1 ||
                !pet_at_home(p.seed) || pet_travel_busy() ||
                !pet_pokemon_set(seed, (uint16_t)dex, shiny != 0))
            {
                printf("@ERR appearance rejected\n");
                return true;
            }
        }
        uint16_t dex;
        bool shiny;
        pet_pokemon_get(seed, &dex, &shiny);
        printf("@PET seed=%s dex=%u shiny=%d roster=%d hunger=%ld energy=%ld joy=%ld xp=%ld asleep=%ld\n",
               p.seed, dex, shiny, pet_pokemon_count(), (long)p.hunger, (long)p.energy,
               (long)p.joy, (long)p.xp, (long)p.asleep);
        printf("@OK pet\n");
        return true;
    }
    if (strncmp(line, "orbi", 4) == 0 && (line[4] == 0 || line[4] == ' '))
    {
        /* "orbi <code>": the genome's traits (checked against tools/orbi_vectors.json);
         * "orbi show [code]": the creature on screen (the father without a code). */
        const char *arg = line[4] ? line + 5 : "";
        bool show = strncmp(arg, "show", 4) == 0;
        const char *code = show ? (arg[4] ? arg + 5 : "") : arg;
        uint64_t seed = 0x0d0000bb00000245ULL;
        if (code[0] != 0 && !orbi_parse(code, &seed))
        {
            printf("@ERR orbi code: 16 hex digits\n");
            return true;
        }
        if (show)
        {
            cJSON *j = cJSON_CreateObject();
            cJSON_AddStringToObject(j, "scene", "orbi");
            cJSON_AddStringToObject(j, "action", "enter");
            cJSON_AddStringToObject(j, "mood", "happy");
            cJSON_AddNumberToObject(j, "hold_ms", 60000);
            if (code[0] != 0)
            {
                cJSON_AddStringToObject(j, "seed", code);
            }
            printf(link_scene_post(j) ? "@OK orbi show\n" : "@ERR orbi show\n");
            return true;
        }
        orbi_genome_t g;
        orbi_genome(seed, &g);
        char c[17];
        orbi_code(seed, c);
        printf("@ORBI seed=%s caps=%u hue=%u body=%u stretch=%u ear=%u crest=%u hue2=%u eyes=%u eye_gap=%u eye_size=%u "
               "pattern=%u cheeks=%d stats=%u,%u,%u,%u,%u name=%s\n",
               c, g.caps, g.hue, g.body, g.stretch, g.ear, g.crest, g.hue2, g.eyes, g.eye_gap, g.eye_size, g.pattern,
               g.cheeks, g.curiosity, g.energy, g.sociability, g.appetite, g.playfulness, g.name);
        return true;
    }
    if (strcmp(line, "beep") == 0)
    {
        sound_notification();   /* speaker check on a new board */
        printf("@OK beep\n");
        return true;
    }
    if (strcmp(line, "sd off") == 0 || strcmp(line, "sd on") == 0)
    {
        /* Exercise the card-less paths (market straight into the slot) on a board with a card. */
        if (line[4] == 'f') sdcard_unmount();
        else sdcard_mount();
        printf("@SD mounted=%d\n", sdcard_is_mounted());
        return true;
    }
    if (strcmp(line, "tone") == 0)
    {
        /* 3 s at 880 Hz, loud: time to probe the PA pin while it plays. */
        printf(sys_task_create(tone_task, "tone", 4096, NULL, 4, 0) ? "@OK tone\n" : "@ERR tone\n");
        return true;
    }
#if CONFIG_WATCH_ECOS_PAIRING
    if (strncmp(line, "ecos", 4) == 0 && (line[4] == 0 || line[4] == ' '))
    {
        /* ECOS pairing from the bench: "ecos start [ip]", "ecos confirm", "ecos cancel", "ecos". */
        const char *op = line[4] ? line + 5 : "";
        ecos_snapshot_t *e = malloc(sizeof(*e));
        if (e == NULL) return true;
        if (strncmp(op, "start", 5) == 0)
            printf("@ECOSREQUEST %s\n", esp_err_to_name(ecos_pair_start(op[5] == ' ' ? op + 6 : NULL)));
        else if (strcmp(op, "confirm") == 0)
        {
            ecos_snapshot(e);
            printf("@ECOSREQUEST %s\n", esp_err_to_name(ecos_pair_confirm(e->candidate_id, e->code)));
        }
        else if (strcmp(op, "cancel") == 0) ecos_pair_cancel();
        ecos_snapshot(e);
        printf("@ECOS running=%d comparing=%d local=%d remote=%d saved=%d left=%u code=%06lu candidate=%s/%s peers=%d msg=%s\n",
               e->running, e->comparing, e->local_ok, e->remote_ok, e->saved, e->seconds_left, (unsigned long)e->code,
               e->candidate, e->candidate_id, e->count, e->message);
        for (int i = 0; i < e->count; i++) printf("@ECOSPEER %s %s\n", e->peers[i].id, e->peers[i].name);
        free(e);
        return true;
    }
#endif
    if (strncmp(line, "market", 6) != 0 || (line[6] != 0 && line[6] != ' ')) return false;
    char *cmd = strtok(line, " ");
    if (strcmp(cmd, "market") == 0)
    {
        char *operation = strtok(NULL, " ");
        if (operation != NULL && strcmp(operation, "refresh") == 0)
            printf("@MARKETREQUEST %s\n", esp_err_to_name(market_refresh()));
        else if (operation != NULL && strcmp(operation, "install") == 0)
            printf("@MARKETREQUEST %s\n", esp_err_to_name(market_install(strtok(NULL, " "))));
        else if (operation != NULL && strcmp(operation, "remove") == 0)
            printf("@MARKETREQUEST %s\n", esp_err_to_name(market_remove(strtok(NULL, " "))));
        else if (operation != NULL && strcmp(operation, "cancel") == 0) market_cancel();
        else if (operation != NULL && strcmp(operation, "selftest") == 0)
            printf("@MARKETTESTS %s\n", market_catalog_selftest() ? "PASS" : "FAIL");
        else if (operation != NULL && strcmp(operation, "dev") == 0)
        {
            const char *value = strtok(NULL, " ");
            market_set_development(value != NULL && strcmp(value, "1") == 0);
        }
        else if (operation != NULL && strcmp(operation, "list") == 0)
        {
            market_entry_t *entry = malloc(sizeof(*entry));
            if (entry != NULL)
            {
                for (int i = 0; market_entry(i, entry); i++)
                    printf("@MARKETAPP %s %s %s installed=%d version=%s\n", entry->id,
                           entry->version, entry->channel, entry->installed, entry->installed_version);
                free(entry);
            }
        }
        market_status_t status;
        market_status(&status);
        printf("@MARKET phase=%d progress=%d busy=%d count=%d revision=%lu message=%s\n",
               status.phase, status.percent, status.busy, market_count(),
               (unsigned long)status.revision, status.message);
        fflush(stdout);
        return true;
    }

    return false;
}
#endif
