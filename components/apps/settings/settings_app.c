#include "apps_internal.h"

#include "core/clock.h"
#include "core/sys.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/state.h"
#include "hardware/board.h"
#include "hardware/pmu.h"
#include "hardware/sdcard.h"
#include "companion/ble_companion.h"
#include "services/sound.h"
#include "services/time_sync.h"
#include "services/wifi.h"
#include "ui/ui.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* Edit one settings field: copy, change, save. */
#define EDIT_SETTINGS(statement)            \
    do                                      \
    {                                       \
        settings_t s_ = *settings_get();    \
        settings_t *s = &s_;                \
        statement;                          \
        settings_save(s);                   \
    } while (0)

static bool switch_on(lv_event_t *e)
{
    return lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
}

/* ================================================================ main */

static void main_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    lv_obj_t *page = ui_page(screen, "Impostazioni");
    ui_row(page, LV_SYMBOL_LIST, UI_COLOR_GREEN, "Tutte le app", NULL, apps_open_cb, (void *)"launcher.all");
    ui_row(page, LV_SYMBOL_DOWNLOAD, UI_COLOR_PURPLE, "Marketplace", NULL, apps_open_cb, (void *)"market");
    ui_row(page, LV_SYMBOL_IMAGE, UI_COLOR_BLUE, "Display", NULL, apps_open_cb, (void *)"settings.display");
    ui_row(page, LV_SYMBOL_VOLUME_MAX, UI_COLOR_PINK, "Suoni", NULL, apps_open_cb, (void *)"settings.sound");
    ui_row(page, LV_SYMBOL_WIFI, UI_COLOR_BLUE, "Wi-Fi", NULL, apps_open_cb, (void *)"settings.wifi");
    ui_row(page, LV_SYMBOL_BLUETOOTH, UI_COLOR_INDIGO, "Bluetooth", NULL, apps_open_cb, (void *)"settings.bluetooth");
    ui_row(page, LV_SYMBOL_LOOP, UI_COLOR_ORANGE, "Ora e data", NULL, apps_open_cb, (void *)"settings.time");
    ui_row(page, LV_SYMBOL_DOWNLOAD, UI_COLOR_GREEN, "Aggiornamento", NULL, apps_open_cb, (void *)"settings.update");
    ui_row(page, LV_SYMBOL_DRIVE, UI_COLOR_GRAY, "Info", NULL, apps_open_cb, (void *)"settings.about");
    ui_row(page, LV_SYMBOL_POWER, UI_COLOR_RED, "Alimentazione", NULL, apps_open_cb, (void *)"system.power");
}

const app_t settings_app = {
    .id = "settings",
    .name = "Impostazioni",
    .icon = LV_SYMBOL_SETTINGS,
    .color = UI_COLOR_GRAY,
    .create = main_create,
};

/* ============================================================= display */

static const uint8_t s_timeouts[] = {10, 15, 30, 60, 120};
static const uint32_t s_accents[] = {0x3D8BFF, 0xFF375F, 0x30D158, 0xFF9F0A, 0xBF5AF2, 0x40C8E0, 0xFFD60A};

static void set_brightness(lv_event_t *e)
{
    int value = lv_slider_get_value(lv_event_get_target_obj(e));
    EDIT_SETTINGS(s->brightness = (uint8_t)value);
    power_settings_changed();
}

static void cycle_timeout(lv_event_t *e)
{
    uint8_t current = settings_get()->screen_timeout_s;
    uint8_t next = s_timeouts[0];

    for (size_t i = 0; i < sizeof(s_timeouts); i++)
    {
        if (s_timeouts[i] > current)
        {
            next = s_timeouts[i];
            break;
        }
    }

    EDIT_SETTINGS(s->screen_timeout_s = next);
    lv_label_set_text_fmt(ui_row_value(lv_event_get_current_target_obj(e)), "%u s", next);
}

static void cycle_cards(lv_event_t *e)
{
    uint8_t current = settings_get()->launcher_cards;
    uint8_t next = current == 4 ? 6 : current == 6 ? 9 : 4;
    EDIT_SETTINGS(s->launcher_cards = next);
    lv_label_set_text_fmt(ui_row_value(lv_event_get_current_target_obj(e)), "%u", next);
}

static void cycle_accent(lv_event_t *e)
{
    uint32_t current = settings_get()->accent_color;
    size_t count = sizeof(s_accents) / sizeof(s_accents[0]);
    size_t next = 0;

    for (size_t i = 0; i < count; i++)
    {
        if (s_accents[i] == current)
        {
            next = (i + 1) % count;
        }
    }

    EDIT_SETTINGS(s->accent_color = s_accents[next]);
    lv_port_set_focus_color(lv_color_hex(s_accents[next]));
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    lv_obj_set_style_bg_color(lv_obj_get_child(row, 0), lv_color_hex(s_accents[next]), 0);
    ui_toast("Colore applicato alle schermate che apri");
}

static void set_aod(lv_event_t *e)
{
    bool on = switch_on(e);
    EDIT_SETTINGS(s->always_on = on);
}

static void set_tap(lv_event_t *e)
{
    bool on = switch_on(e);
    EDIT_SETTINGS(s->tap_to_wake = on);
}

static void set_wake_notify(lv_event_t *e)
{
    bool on = switch_on(e);
    EDIT_SETTINGS(s->wake_on_notification = on);
}

static void set_border_progress(lv_event_t *e)
{
    EDIT_SETTINGS(s->border_progress = switch_on(e));
    ui_toast("Applicato alle schermate che apri");
}

static void set_knob_focus(lv_event_t *e)
{
    bool on = switch_on(e);
    EDIT_SETTINGS(s->knob_focus = on);
    lv_port_set_focus_mode(on);
}

static void display_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    const settings_t *st = settings_get();
    char value[16];

    lv_obj_t *page = ui_page(screen, "Display");
    ui_slider_row(page, LV_SYMBOL_IMAGE, "Luminosità", 10, 255, st->brightness, set_brightness, NULL);
    snprintf(value, sizeof(value), "%u s", st->screen_timeout_s);
    ui_row(page, LV_SYMBOL_EYE_CLOSE, UI_COLOR_INDIGO, "Spegnimento", value, cycle_timeout, NULL);
    ui_switch_row(page, LV_SYMBOL_EYE_OPEN, UI_COLOR_PURPLE, "Always on", st->always_on, set_aod, NULL);
    ui_switch_row(page, LV_SYMBOL_LOOP, UI_COLOR_TEAL, "Progresso sul bordo", st->border_progress, set_border_progress, NULL);
    ui_switch_row(page, LV_SYMBOL_UP, UI_COLOR_BLUE, "Tocca per accendere", st->tap_to_wake, set_tap, NULL);
    ui_switch_row(page, LV_SYMBOL_BELL, UI_COLOR_ORANGE, "Accendi con notifiche", st->wake_on_notification,
                  set_wake_notify, NULL);
    ui_row(page, " ", st->accent_color, "Colore principale", NULL, cycle_accent, NULL);
    snprintf(value, sizeof(value), "%u", st->launcher_cards);
    ui_row(page, LV_SYMBOL_LIST, UI_COLOR_BLUE, "App per pagina", value, cycle_cards, NULL);

    if (board_info()->has_knob)
    {
        ui_switch_row(page, LV_SYMBOL_SHUFFLE, UI_COLOR_TEAL, "Rotella: focus", st->knob_focus, set_knob_focus, NULL);
    }

    ui_text(page, "Always on: l'ora resta visibile, attenuata (consuma un po' di più).", true);

    if (board_info()->has_knob)
    {
        ui_text(page, "Rotella: gira per scorrere, con focus scegli le voci. Tocca per aprire; "
                      "scorri dal bordo sinistro per tornare. Sulle rotelle con click puoi anche "
                      "premere per aprire e fare doppio click per cambiare modalità.", true);
    }
}

const app_t settings_display_app = {
    .id = "settings.display",
    .name = "Display",
    .icon = LV_SYMBOL_IMAGE,
    .color = UI_COLOR_BLUE,
    .flags = APP_FLAG_HIDDEN,
    .create = display_create,
};

/* =============================================================== sound */

static void set_volume(lv_event_t *e)
{
    int value = lv_slider_get_value(lv_event_get_target_obj(e));
    EDIT_SETTINGS(s->volume = (uint8_t)value);
}

static void set_clicks(lv_event_t *e)
{
    bool on = switch_on(e);
    EDIT_SETTINGS(s->key_sounds = on);
}

static void test_sound(lv_event_t *e)
{
    (void)e;
    sound_notification();
}

static void sound_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    lv_obj_t *page = ui_page(screen, "Suoni");
    ui_slider_row(page, LV_SYMBOL_VOLUME_MAX, "Volume", 0, 100, settings_get()->volume, set_volume, NULL);
    ui_switch_row(page, LV_SYMBOL_KEYBOARD, UI_COLOR_GRAY, "Suono tasti", settings_get()->key_sounds, set_clicks, NULL);
    ui_row(page, LV_SYMBOL_PLAY, UI_COLOR_PINK, "Prova suono", NULL, test_sound, NULL);
}

const app_t settings_sound_app = {
    .id = "settings.sound",
    .name = "Suoni",
    .icon = LV_SYMBOL_VOLUME_MAX,
    .color = UI_COLOR_PINK,
    .flags = APP_FLAG_HIDDEN,
    .create = sound_create,
};

/* ================================================================ wifi */

typedef struct
{
    lv_obj_t *page;
    lv_obj_t *status;
    lv_obj_t *results;
    wifi_ap_t found[WIFI_SCAN_MAX];
    int count;
    bool busy;
    char pending_ssid[33];
} wifi_ui_t;

static wifi_ui_t *s_wifi = NULL;
static volatile int s_wifi_generation = 0;   /* changes when the page closes */

static void wifi_status_update(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    char ssid[33];
    char ip[20] = "";
    wifi_current_ssid(ssid, sizeof(ssid));
    wifi_get_ip(ip, sizeof(ip));

    switch (state_get(STATE_WIFI))
    {
        case STATE_WIFI_CONNECTED:
            lv_label_set_text_fmt(s_wifi->status, LV_SYMBOL_OK "  %s  ·  %s", ssid, ip);
            break;
        case STATE_WIFI_CONNECTING:
            lv_label_set_text(s_wifi->status, "Connessione...");
            break;
        case STATE_WIFI_AP:
            lv_label_set_text(s_wifi->status, "Access point attivo");
            break;
        default:
            lv_label_set_text(s_wifi->status, "Spento (si accende solo quando serve)");
            break;
    }
}

static void wifi_connect_task(void *arg);

static void password_entered(const char *password, void *user_data)
{
    (void)user_data;

    if (password == NULL || s_wifi == NULL)
    {
        return;
    }

    char *job = malloc(33 + 65);

    if (job != NULL)
    {
        snprintf(job, 33, "%s", s_wifi->pending_ssid);
        snprintf(job + 33, 65, "%s", password);
        sys_task_create(wifi_connect_task, "wifi_join", 4096, job, 3, SYS_CORE_ANY);
    }
}

static void connect_done(void *arg)
{
    bool ok = arg != NULL;
    ui_toast(ok ? "Connesso e salvato" : "Connessione o salvataggio non riuscito");
}

static void wifi_connect_task(void *arg)
{
    char *job = arg;
    esp_err_t err = wifi_connect_new(job, job + 33, 15000);

    if (err == ESP_OK)
    {
        wifi_release();   /* saved; Wi-Fi turns off again when nobody needs it */
    }

    free(job);
    ui_async(connect_done, err == ESP_OK ? (void *)1 : NULL);
    vTaskDelete(NULL);
}

static void on_network(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    const wifi_ap_t *ap = &s_wifi->found[index];
    snprintf(s_wifi->pending_ssid, sizeof(s_wifi->pending_ssid), "%s", ap->ssid);

    if (ap->open)
    {
        password_entered("", NULL);
    }
    else
    {
        ui_text_input(ap->ssid, "", true, password_entered, NULL);
    }
}

static void show_results(void *arg)
{
    if ((int)(intptr_t)arg != s_wifi_generation || s_wifi == NULL)
    {
        return;   /* the page was closed meanwhile */
    }

    s_wifi->busy = false;
    lv_obj_clean(s_wifi->results);

    for (int i = 0; i < s_wifi->count; i++)
    {
        const wifi_ap_t *ap = &s_wifi->found[i];
        const char *quality = ap->rssi > -60 ? "ottimo" : (ap->rssi > -72 ? "buono" : "debole");
        char value[24];
        snprintf(value, sizeof(value), "%s%s", ap->known ? "salvata · " : "", quality);
        ui_row(s_wifi->results, ap->open ? LV_SYMBOL_WIFI : LV_SYMBOL_EYE_CLOSE,
               ap->known ? UI_COLOR_GREEN : UI_COLOR_BLUE, ap->ssid, value, on_network, (void *)(intptr_t)i);
    }

    if (s_wifi->count == 0)
    {
        ui_text(s_wifi->results, "Nessuna rete trovata.", true);
    }
}

static void scan_task(void *arg)
{
    int generation = (int)(intptr_t)arg;
    wifi_ap_t *found = calloc(WIFI_SCAN_MAX, sizeof(wifi_ap_t));
    int count = found != NULL ? wifi_scan(found, WIFI_SCAN_MAX) : 0;

    lv_port_lock();

    if (generation == s_wifi_generation && s_wifi != NULL)
    {
        memcpy(s_wifi->found, found, sizeof(wifi_ap_t) * (size_t)count);
        s_wifi->count = count;
    }

    lv_port_unlock();

    free(found);
    ui_async(show_results, (void *)(intptr_t)generation);
    vTaskDelete(NULL);
}

static void on_scan(lv_event_t *e)
{
    (void)e;

    if (s_wifi->busy)
    {
        return;
    }

    s_wifi->busy = true;
    lv_obj_clean(s_wifi->results);
    ui_text(s_wifi->results, "Ricerca reti...", true);
    sys_task_create(scan_task, "wifi_scan", 4096, (void *)(intptr_t)s_wifi_generation, 3, SYS_CORE_ANY);
}

static void forget_confirmed(bool yes, void *user_data)
{
    char *ssid = user_data;

    if (yes)
    {
        wifi_known_remove(ssid);
        ui_toast("Rete dimenticata");
    }

    free(ssid);
}

static void on_known(lv_event_t *e)
{
    const char *ssid = lv_label_get_text(lv_obj_get_child(lv_event_get_current_target_obj(e), 1));
    char *copy = strdup(ssid);

    if (copy != NULL)
    {
        ui_confirm("Dimenticare la rete?", ssid, "Dimentica", UI_COLOR_RED, forget_confirmed, copy);
    }
}

static void wifi_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    s_wifi = calloc(1, sizeof(wifi_ui_t));

    if (s_wifi == NULL)
    {
        return;
    }

    s_wifi->page = ui_page(screen, "Wi-Fi");
    s_wifi->status = ui_text(s_wifi->page, "", true);

    ui_button(s_wifi->page, LV_SYMBOL_REFRESH "  Cerca reti", UI_COLOR_BLUE, on_scan, NULL);

    s_wifi->results = lv_obj_create(s_wifi->page);
    lv_obj_remove_style_all(s_wifi->results);
    lv_obj_set_size(s_wifi->results, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_wifi->results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi->results, 10, 0);

    wifi_known_t known[WIFI_KNOWN_MAX];
    int count = wifi_known_list(known, WIFI_KNOWN_MAX);

    if (count > 0)
    {
        ui_section(s_wifi->page, "RETI SALVATE  ·  tocca per dimenticare");

        for (int i = 0; i < count; i++)
        {
            ui_row(s_wifi->page, LV_SYMBOL_WIFI, UI_COLOR_GREEN, known[i].ssid, NULL, on_known, NULL);
        }
    }

    ui_text(s_wifi->page, board_info()->has_sdcard
                ? "Le reti si possono aggiungere anche dalla pagina web o in /config/wifi.txt sulla SD."
                : "Le reti vengono salvate nella memoria interna. Puoi aggiungerle anche dalla pagina web.", true);

    lv_subject_add_observer_obj(state_subject(STATE_WIFI), wifi_status_update, s_wifi->page, NULL);
}

static void wifi_destroy(void)
{
    s_wifi_generation++;
    free(s_wifi);
    s_wifi = NULL;
}

const app_t settings_wifi_app = {
    .id = "settings.wifi",
    .name = "Wi-Fi",
    .icon = LV_SYMBOL_WIFI,
    .color = UI_COLOR_BLUE,
    .flags = APP_FLAG_HIDDEN,
    .create = wifi_create,
    .destroy = wifi_destroy,
};

/* =========================================================== bluetooth */

static lv_obj_t *s_ble_status = NULL;

static void ble_status_update(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)observer;
    (void)subject;

    static const char *const texts[] = {"Spento", "In attesa del telefono", "Telefono connesso"};
    int st = state_get(STATE_BLE);
    lv_label_set_text(s_ble_status, texts[st >= 0 && st <= 2 ? st : 0]);
}

static void set_ble(lv_event_t *e)
{
    bool on = switch_on(e);
    EDIT_SETTINGS(s->ble_enabled = on);   /* the companion follows the setting */
}

static void name_entered(const char *text, void *user_data)
{
    (void)user_data;

    if (text != NULL && text[0] != '\0')
    {
        EDIT_SETTINGS(snprintf(s->device_name, sizeof(s->device_name), "%s", text));
        ble_companion_restart();
        ui_toast("Nome aggiornato");
    }
}

static void on_name(lv_event_t *e)
{
    (void)e;
    ui_text_input("Nome Bluetooth", settings_get()->device_name, false, name_entered, NULL);
}

static void on_find_phone(lv_event_t *e)
{
    (void)e;

    if (ble_companion_connected())
    {
        ble_companion_find_phone(true);
        ui_toast("Il telefono sta suonando");
    }
    else
    {
        ui_toast("Telefono non connesso");
    }
}

static void bluetooth_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    lv_obj_t *page = ui_page(screen, "Bluetooth");
    s_ble_status = ui_text(page, "", true);
    ui_switch_row(page, LV_SYMBOL_BLUETOOTH, UI_COLOR_INDIGO, "Bluetooth", settings_get()->ble_enabled, set_ble, NULL);
    ui_row(page, LV_SYMBOL_EDIT, UI_COLOR_GRAY, "Nome", settings_get()->device_name, on_name, NULL);
    ui_row(page, LV_SYMBOL_GPS, UI_COLOR_GREEN, "Trova telefono", NULL, on_find_phone, NULL);
    ui_text(page,
            "Sul telefono Android installa Gadgetbridge e aggiungi questo orologio: arrivano notifiche, "
            "chiamate, meteo, ora e controlli musica. Il nome deve iniziare con \"Bangle.js\".",
            true);

    lv_subject_add_observer_obj(state_subject(STATE_BLE), ble_status_update, page, NULL);
}

const app_t settings_bluetooth_app = {
    .id = "settings.bluetooth",
    .name = "Bluetooth",
    .icon = LV_SYMBOL_BLUETOOTH,
    .color = UI_COLOR_INDIGO,
    .flags = APP_FLAG_HIDDEN,
    .create = bluetooth_create,
};

/* ================================================================ time */

typedef struct
{
    const char *name;
    const char *tz;
} zone_t;

static const zone_t s_zones[] = {
    {"Italia / Europa centrale", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Regno Unito / Portogallo", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europa orientale", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"UTC", "UTC0"},
    {"New York", "EST5EDT,M3.2.0,M11.1.0"},
    {"Los Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"Tokyo", "JST-9"},
};

static lv_obj_t *s_rollers[5];

static const char *zone_name(const char *tz)
{
    for (size_t i = 0; i < sizeof(s_zones) / sizeof(s_zones[0]); i++)
    {
        if (strcmp(s_zones[i].tz, tz) == 0)
        {
            return s_zones[i].name;
        }
    }

    return tz;
}

static void cycle_zone(lv_event_t *e)
{
    size_t count = sizeof(s_zones) / sizeof(s_zones[0]);
    size_t next = 0;

    for (size_t i = 0; i < count; i++)
    {
        if (strcmp(s_zones[i].tz, settings_get()->timezone) == 0)
        {
            next = (i + 1) % count;
        }
    }

    EDIT_SETTINGS(snprintf(s->timezone, sizeof(s->timezone), "%s", s_zones[next].tz));
    clock_set_timezone(s_zones[next].tz);
    lv_label_set_text(ui_row_value(lv_event_get_current_target_obj(e)), s_zones[next].name);
}

static void set_auto_time(lv_event_t *e)
{
    bool on = switch_on(e);
    EDIT_SETTINGS(s->auto_time = on);
}

static void set_24h(lv_event_t *e)
{
    bool on = switch_on(e);
    EDIT_SETTINGS(s->time_24h = on);
}

static void sync_done(void *arg)
{
    ui_toast(arg != NULL ? "Ora sincronizzata" : "Sincronizzazione fallita");
}

static void sync_task(void *arg)
{
    (void)arg;
    esp_err_t err = time_sync_now();
    ui_async(sync_done, err == ESP_OK ? (void *)1 : NULL);
    vTaskDelete(NULL);
}

static void on_sync(lv_event_t *e)
{
    (void)e;
    ui_toast("Sincronizzazione...");
    sys_task_create(sync_task, "ntp_now", 4096, NULL, 3, SYS_CORE_ANY);
}

static void on_set_manual(lv_event_t *e)
{
    (void)e;

    struct tm t = {0};
    t.tm_hour = (int)lv_roller_get_selected(s_rollers[0]);
    t.tm_min = (int)lv_roller_get_selected(s_rollers[1]);
    t.tm_mday = (int)lv_roller_get_selected(s_rollers[2]) + 1;
    t.tm_mon = (int)lv_roller_get_selected(s_rollers[3]);
    t.tm_year = 2025 + (int)lv_roller_get_selected(s_rollers[4]) - 1900;
    t.tm_isdst = -1;

    clock_set_utc(mktime(&t));   /* mktime: local -> UTC */
    ui_toast("Ora impostata");
}

static lv_obj_t *small_roller(lv_obj_t *parent, const char *options, int selected, int32_t width)
{
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    lv_roller_set_selected(r, (uint32_t)selected, LV_ANIM_OFF);
    lv_obj_set_width(r, width);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(UI_COLOR_CARD), LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, ui_accent(), LV_PART_SELECTED);
    return r;
}

static void numbers(char *out, size_t size, int from, int to)
{
    size_t used = 0;

    for (int i = from; i <= to && used + 6 < size; i++)
    {
        used += (size_t)snprintf(out + used, size - used, i == from ? "%02d" : "\n%02d", i);
    }
}

static lv_obj_t *roller_row(lv_obj_t *page)
{
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    return row;
}

static void time_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    const settings_t *st = settings_get();
    lv_obj_t *page = ui_page(screen, "Ora e data");

    ui_switch_row(page, LV_SYMBOL_REFRESH, UI_COLOR_GREEN, "Ora automatica", st->auto_time, set_auto_time, NULL);
    ui_row(page, LV_SYMBOL_DOWNLOAD, UI_COLOR_BLUE, "Sincronizza ora", NULL, on_sync, NULL);
    ui_switch_row(page, LV_SYMBOL_LOOP, UI_COLOR_ORANGE, "Formato 24 ore", st->time_24h, set_24h, NULL);
    ui_row(page, LV_SYMBOL_GPS, UI_COLOR_TEAL, "Fuso orario", zone_name(st->timezone), cycle_zone, NULL);

    ui_section(page, "IMPOSTA A MANO");

    struct tm now;
    clock_local(&now);

    static char hours[24 * 3 + 1];
    static char minutes[60 * 3 + 1];
    static char days[31 * 3 + 1];
    static char months[12 * 3 + 1];
    static char years[11 * 5 + 1];
    numbers(hours, sizeof(hours), 0, 23);
    numbers(minutes, sizeof(minutes), 0, 59);
    numbers(days, sizeof(days), 1, 31);
    numbers(months, sizeof(months), 1, 12);
    snprintf(years, sizeof(years), "2025\n2026\n2027\n2028\n2029\n2030\n2031\n2032\n2033\n2034\n2035");

    lv_obj_t *row1 = roller_row(page);
    s_rollers[0] = small_roller(row1, hours, now.tm_hour, 90);
    s_rollers[1] = small_roller(row1, minutes, now.tm_min, 90);

    lv_obj_t *row2 = roller_row(page);
    s_rollers[2] = small_roller(row2, days, now.tm_mday - 1, 80);
    s_rollers[3] = small_roller(row2, months, now.tm_mon, 80);
    int year = now.tm_year + 1900 - 2025;
    s_rollers[4] = small_roller(row2, years, year >= 0 && year <= 10 ? year : 1, 110);

    ui_button(page, "Imposta", UI_COLOR_ORANGE, on_set_manual, NULL);
    ui_text(page, "Con \"Ora automatica\" l'ora arriva dal telefono (Bluetooth) o da Internet.", true);
}

const app_t settings_time_app = {
    .id = "settings.time",
    .name = "Ora e data",
    .icon = LV_SYMBOL_LOOP,
    .color = UI_COLOR_ORANGE,
    .flags = APP_FLAG_HIDDEN,
    .create = time_create,
};

/* =============================================================== about */

static lv_obj_t *s_about = NULL;
static lv_timer_t *s_about_timer = NULL;

static void about_update(lv_timer_t *timer)
{
    (void)timer;

    char ip[20] = "-";
    char sd[40] = "assente";
    if (!board_info()->has_sdcard)
    {
        snprintf(sd, sizeof(sd), "non supportata");
    }
    uint32_t total_mb = 0;
    uint32_t free_mb = 0;
    wifi_get_ip(ip, sizeof(ip));

    if (sdcard_is_mounted())
    {
        sdcard_space(&total_mb, &free_mb);
        snprintf(sd, sizeof(sd), "%lu MB liberi su %lu", (unsigned long)free_mb, (unsigned long)total_mb);
    }

    int64_t uptime = esp_timer_get_time() / 1000000;

    lv_label_set_text_fmt(s_about,
                          "Scheda: %s\n"
                          "Variante: %s · %ux%u %s · %s\n"
                          "Firmware: %s · %s (ESP-IDF %s)\n\n"
                          "Batteria: %d%%  %d mV%s\n"
                          "RAM interna libera: %u KB (min %u)\n"
                          "PSRAM libera: %u KB\n"
                          "SD: %s\n"
                          "IP: %s\n"
                          "Acceso da: %lldh %02lldm\n"
                          "Ultimo avvio: %s",
                          board_info()->name, board_info()->product, board_info()->width, board_info()->height,
                          board_info()->round ? "rotondo" : "rettangolare",
                          board_info()->has_knob ? "touch + rotella" : "touch",
                          esp_app_get_description()->version, esp_app_get_description()->project_name,
                          esp_get_idf_version(),
                          pmu_battery_percent(), pmu_battery_mv(), pmu_is_charging() ? " (in carica)" : "",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                          sd, ip, uptime / 3600, (uptime / 60) % 60, sys_last_reset_text());
}

static void about_create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    lv_obj_t *page = ui_page(screen, "Info");
    s_about = ui_text(page, "", false);
    lv_obj_set_style_text_font(s_about, UI_FONT_SMALL, 0);
    s_about_timer = lv_timer_create(about_update, 2000, NULL);
    about_update(NULL);
}

static void about_destroy(void)
{
    lv_timer_delete(s_about_timer);
    s_about_timer = NULL;
}

const app_t settings_about_app = {
    .id = "settings.about",
    .name = "Info",
    .icon = LV_SYMBOL_DRIVE,
    .color = UI_COLOR_GRAY,
    .flags = APP_FLAG_HIDDEN,
    .create = about_create,
    .destroy = about_destroy,
};
