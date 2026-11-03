/*
 * Remote Control home screen.
 *
 *   status        "Bluetooth: connesso" (updated live)
 *   MODALITÀ      keyboards, mouse, media, presentations, meetings and gamepad
 *   COLLEGAMENTO  Bluetooth / USB, options
 *
 * Changing the transport saves it and restarts the app: Bluetooth and USB
 * stacks are never torn down at runtime, which is the reliable way.
 */

#include "remote.h"
#include "hardware/imu.h"
#include "core/lv_port.h"
#include "ui/keyboard_layout.h"

#include "extapp_sdk.h"
#include "hardware/board.h"
#include "ui/ui.h"

#include <stdint.h>

typedef struct
{
    const char *name;
    const char *label; /* short name for the switcher tiles */
    const char *icon;
    uint32_t color;
    const app_t *app; /* NULL = not available yet */
} remote_mode_t;

static const remote_mode_t s_modes[] = {
    {"PC (Deck)", "Deck", LV_SYMBOL_HOME, UI_COLOR_GREEN, &remote_deck_app},
    {"Tastiera", "Tastiera", LV_SYMBOL_EDIT, UI_COLOR_BLUE, &keyboard_app},
    {"Tastiera PC", "Tasti PC", LV_SYMBOL_KEYBOARD, UI_COLOR_TEAL, &remote_pc_keyboard_app},
    {"Mouse / touchpad", "Mouse", LV_SYMBOL_GPS, UI_COLOR_TEAL, &remote_mouse_app},
    {"Air mouse", "Air mouse", LV_SYMBOL_WIFI, UI_COLOR_ORANGE, &remote_air_app},
    {"Controllo media", "Media", LV_SYMBOL_AUDIO, UI_COLOR_PINK, &remote_media_app},
    {"Presentazioni", "Slide", LV_SYMBOL_IMAGE, UI_COLOR_ORANGE, &remote_presenter_app},
    {"Teams / Zoom", "Riunioni", LV_SYMBOL_CALL, UI_COLOR_INDIGO, &remote_meeting_app},
    {"Gamepad", "Gamepad", LV_SYMBOL_SHUFFLE, UI_COLOR_PURPLE, &remote_gamepad_app},
    {"Scorciatoie", "Scorciatoie", LV_SYMBOL_LIST, UI_COLOR_BLUE, &remote_shortcuts_app},
};

static lv_obj_t *s_status;
static uint32_t s_seen_version = UINT32_MAX;
static hid_link_state_t s_seen_state = HID_LINK_OFF;

static void refresh_status(lv_timer_t *timer)
{
    (void)timer;

    if (hid_link_version() == s_seen_version && hid_link_state() == s_seen_state)
    {
        return;
    }

    s_seen_version = hid_link_version();
    s_seen_state = hid_link_state();

    lv_label_set_text(s_status, remote_status_text());
    lv_obj_set_style_text_color(s_status,
                                ui_color(s_seen_state == HID_LINK_CONNECTED ? UI_COLOR_GREEN : UI_COLOR_ORANGE), 0);
}

static void open_mode(lv_event_t *e)
{
    const remote_mode_t *mode = lv_event_get_user_data(e);

    if (mode->app == NULL)
    {
        ui_toast("In arrivo in un prossimo aggiornamento");
        return;
    }

    app_open_app(mode->app, NULL);
}

static void transport_confirmed(bool confirmed, void *user_data)
{
    if (!confirmed)
    {
        return;
    }

    remote_settings_t s = *remote_settings();
    s.transport = (uint8_t)(uintptr_t)user_data;
    remote_settings_save(&s);
    extapp_restart_self();
}

void remote_change_transport(lv_event_t *e)
{
    (void)e;
    bool to_usb = remote_settings()->transport != HID_LINK_USB;

    ui_confirm(to_usb ? "Passa a USB" : "Passa a Bluetooth",
               to_usb ? "L'app si riavvia come tastiera USB. Collega il cavo al computer."
                      : "L'app si riavvia come tastiera Bluetooth. Associala dal computer o dal telefono.",
               "Riavvia", UI_COLOR_BLUE, transport_confirmed,
               (void *)(uintptr_t)(to_usb ? HID_LINK_USB : HID_LINK_BLE));
}

static void open_hub(lv_event_t *e)
{
    (void)e;
    app_open_app(&remote_hub_app, NULL);
}

static void open_options(lv_event_t *e)
{
    (void)e;
    app_open_app(&remote_options_app, NULL);
}

static void leave(lv_event_t *e)
{
    (void)e;
    extapp_return_to_launcher();
}

void remote_home_create(lv_obj_t *screen)
{
    lv_obj_t *page = ui_page(screen, "Remote");

    s_status = ui_text(page, remote_status_text(), false);
    lv_obj_set_style_text_font(s_status, UI_FONT_SMALL, 0);

    ui_section(page, "MODALITÀ");

    for (size_t i = 0; i < sizeof(s_modes) / sizeof(s_modes[0]); i++)
    {
        const remote_mode_t *mode = &s_modes[i];
        if (mode->app == &remote_air_app && !imu_supported())
            continue; /* no gyroscope on this board */
        ui_row(page, mode->icon, mode->app ? mode->color : UI_COLOR_GRAY, mode->name, mode->app ? NULL : "presto",
               open_mode, (void *)mode);
    }

    ui_section(page, "COLLEGAMENTO");

    if (board_info()->has_usb_otg)
    {
        ui_row(page, LV_SYMBOL_BLUETOOTH, UI_COLOR_INDIGO, "Via",
               remote_settings()->transport == HID_LINK_USB ? "USB" : "Bluetooth", remote_change_transport, NULL);
    }
    else
    {
        ui_text(page, remote_device_name(), true);
    }

    ui_row(page, LV_SYMBOL_SHUFFLE, UI_COLOR_PURPLE, "Hub multi-orologio", remote_hub_role_text(), open_hub, NULL);
    ui_row(page, LV_SYMBOL_SETTINGS, UI_COLOR_GRAY, "Opzioni", NULL, open_options, NULL);
    ui_row(page, LV_SYMBOL_LEFT, UI_COLOR_RED, "Esci", NULL, leave, NULL);

    ui_text(page, "Associa Remote dalle impostazioni Bluetooth del computer. Esci torna al launcher.", true);

    refresh_status(NULL);
    lv_timer_create(refresh_status, 300, NULL); /* the home screen is never deleted */
}

/* ------------------------------------------------------------ mode switcher
 *
 * Fixed spatial map, the same on every board: ten modes, Options and Home.
 * 3 columns in portrait, 4 in landscape; never scrolls. The mode the switcher
 * was opened from is lit, and tapping it again simply goes back to it. */

static const app_t *s_previous_mode;

static void choose_mode(lv_event_t *e)
{
    const app_t *target = lv_event_get_user_data(e);
    if (target == s_previous_mode)
    {
        app_back();
        return;
    }
    hid_link_release_all();
    app_close_others(NULL);
    if (target != NULL)
        app_open_app(target, NULL);
}

static void switcher_tile(lv_obj_t *screen, ui_key_rect_t r, const char *icon, const char *text, uint32_t color,
                          const app_t *target, bool disabled)
{
    bool current = target != NULL && target == s_previous_mode;
    lv_obj_t *button = lv_obj_create(screen);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, r.x + 3, r.y + 3);
    lv_obj_set_size(button, r.w - 6, r.h - 6);
    lv_obj_set_scrollable(button, false);
    lv_obj_set_style_radius(button, 16, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, ui_color(current ? UI_COLOR_CARD_HI : UI_COLOR_CARD), 0);
    lv_obj_set_style_bg_color(button, ui_color(UI_COLOR_CARD_HI), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, current ? 2 : 1, 0);
    lv_obj_set_style_border_color(button, current ? ui_color(color) : ui_color(UI_COLOR_SEPARATOR), 0);
    lv_obj_set_style_transform_scale(button, 240, LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_x(button, (r.w - 6) / 2, 0);
    lv_obj_set_style_transform_pivot_y(button, (r.h - 6) / 2, 0);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    lv_port_set_focusable(button);

    lv_obj_t *glyph = lv_label_create(button);
    lv_label_set_text(glyph, icon);
    lv_obj_set_style_text_font(glyph, UI_FONT_LARGE, 0);
    lv_obj_set_style_text_color(glyph, ui_color(color), 0);
    lv_obj_align(glyph, LV_ALIGN_CENTER, 0, -(r.h - 6) / 7);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, r.w - 12);
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, ui_color(current ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -(r.h - 6) / 9);

    if (disabled)
        lv_obj_add_state(button, LV_STATE_DISABLED);
    else
        lv_obj_add_event_cb(button, choose_mode, LV_EVENT_CLICKED, (void *)target);
}

static void switcher_create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_set_scrollable(screen, false);
    int32_t w = remote_screen_w(), h = remote_screen_h();
    bool round = board_info()->round;
    int cols = remote_landscape() ? 4 : 3, rows = 12 / cols;

    ui_key_rect_t head = ui_keyboard_band(w, h, round, round ? h * 9 / 100 : 8, 36, 8);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Modalità");
    lv_obj_set_style_text_font(title, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(title, ui_color(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_width(title, head.w);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(title, head.x, head.y + (head.h - lv_font_get_line_height(UI_FONT_BODY)) / 2);

    int32_t top = head.y + head.h + 2, bottom = round ? h * 91 / 100 : h - 6;
    for (int i = 0; i < 12; ++i)
    {
        int row = i / cols, col = i % cols;
        int32_t y0 = top + (bottom - top) * row / rows, y1 = top + (bottom - top) * (row + 1) / rows;
        ui_key_rect_t band = ui_keyboard_band(w, h, round, y0, y1 - y0, 8);
        int32_t x0 = band.x + band.w * col / cols, x1 = band.x + band.w * (col + 1) / cols;
        ui_key_rect_t r = {x0, band.y, x1 - x0, band.h};
        if (i < 10)
        {
            const remote_mode_t *mode = &s_modes[i];
            switcher_tile(screen, r, mode->icon, mode->label, mode->color, mode->app,
                          mode->app == &remote_air_app && !imu_supported());
        }
        else if (i == 10)
            switcher_tile(screen, r, LV_SYMBOL_SETTINGS, "Opzioni", UI_COLOR_GRAY, &remote_options_app, false);
        else
            switcher_tile(screen, r, LV_SYMBOL_HOME, "Remote", UI_COLOR_TEXT, NULL, false);
    }
}

const app_t remote_switcher_app = {.id = "remote.switcher",
                                 .name = "Modalità",
                                 .flags = APP_FLAG_HIDDEN | APP_FLAG_KEEP_SCREEN_ON,
                                 .create = switcher_create};

void remote_switcher_open(lv_event_t *e)
{
    (void)e;
    if (app_current() == &remote_switcher_app)
    {
        app_back();
        return;
    }
    s_previous_mode = app_current();
    hid_link_release_all();
    app_open_app(&remote_switcher_app, NULL);
}

/* Top-left of the header band every mode shares. */
static lv_obj_t *switcher_button_at(lv_obj_t *screen, ui_key_rect_t band)
{
    lv_obj_t *button = lv_obj_create(screen);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, band.x + 3, band.y + 3);
    lv_obj_set_size(button, 44, band.h - 6);
    lv_obj_set_scrollable(button, false);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, ui_color(UI_COLOR_CARD_HI), 0);
    lv_obj_set_style_bg_color(button, ui_accent(), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(button, 8);
    lv_obj_add_event_cb(button, remote_switcher_open, LV_EVENT_CLICKED, NULL);
    lv_port_set_focusable(button);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(label, ui_accent(), 0);
    lv_obj_center(label);
    return button;
}

ui_key_rect_t remote_header_band(void)
{
    int32_t h = remote_screen_h();
    bool round = board_info()->round;
    return ui_keyboard_band(remote_screen_w(), h, round, round ? h * 9 / 100 : 6, 44, 8);
}

lv_obj_t *remote_switcher_button(lv_obj_t *screen)
{
    return switcher_button_at(screen, remote_header_band());
}

/* A scrolling list page with the switcher: the title moves clear of the button. */
lv_obj_t *remote_page(lv_obj_t *screen, const char *title)
{
    lv_obj_t *page = ui_page(screen, title);
    lv_obj_t *label = lv_obj_get_child(page, 0);
    lv_obj_set_style_pad_left(label, 56, 0);
    lv_obj_set_style_pad_right(label, 56, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    remote_switcher_button(screen);
    return page;
}
