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

#include "extapp_sdk.h"
#include "hardware/board.h"
#include "ui/ui.h"

#include <stdint.h>

typedef struct
{
    const char *name;
    const char *icon;
    uint32_t color;
    const app_t *app; /* NULL = not available yet */
} remote_mode_t;

static const remote_mode_t s_modes[] = {
    {"Tastiera", LV_SYMBOL_KEYBOARD, UI_COLOR_BLUE, &keyboard_app},
    {"Tastiera PC", LV_SYMBOL_KEYBOARD, UI_COLOR_TEAL, &remote_pc_keyboard_app},
    {"Mouse / touchpad", LV_SYMBOL_GPS, UI_COLOR_TEAL, &remote_mouse_app},
    {"Controllo media", LV_SYMBOL_AUDIO, UI_COLOR_PINK, &remote_media_app},
    {"Presentazioni", LV_SYMBOL_IMAGE, UI_COLOR_ORANGE, &remote_presenter_app},
    {"Teams / Zoom", LV_SYMBOL_CALL, UI_COLOR_INDIGO, &remote_meeting_app},
    {"Gamepad", LV_SYMBOL_SHUFFLE, UI_COLOR_PURPLE, &remote_gamepad_app},
    {"Scorciatoie", LV_SYMBOL_LIST, UI_COLOR_BLUE, &remote_shortcuts_app},
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

static void change_transport(lv_event_t *e)
{
    (void)e;
    bool to_usb = remote_settings()->transport != HID_LINK_USB;

    ui_confirm(to_usb ? "Passa a USB" : "Passa a Bluetooth",
               to_usb ? "L'app si riavvia come tastiera USB. Collega il cavo al computer."
                      : "L'app si riavvia come tastiera Bluetooth. Associala dal computer o dal telefono.",
               "Riavvia", UI_COLOR_BLUE, transport_confirmed,
               (void *)(uintptr_t)(to_usb ? HID_LINK_USB : HID_LINK_BLE));
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
        ui_row(page, mode->icon, mode->app ? mode->color : UI_COLOR_GRAY, mode->name, mode->app ? NULL : "presto",
               open_mode, (void *)mode);
    }

    ui_section(page, "COLLEGAMENTO");

    if (board_info()->has_usb_otg)
    {
        ui_row(page, LV_SYMBOL_BLUETOOTH, UI_COLOR_INDIGO, "Via",
               remote_settings()->transport == HID_LINK_USB ? "USB" : "Bluetooth", change_transport, NULL);
    }
    else
    {
        ui_text(page, remote_device_name(), true);
    }

    ui_row(page, LV_SYMBOL_SETTINGS, UI_COLOR_GRAY, "Opzioni", NULL, open_options, NULL);
    ui_row(page, LV_SYMBOL_LEFT, UI_COLOR_RED, "Esci", NULL, leave, NULL);

    ui_text(page, "Associa Remote dalle impostazioni Bluetooth del computer. Esci torna al launcher.", true);

    refresh_status(NULL);
    lv_timer_create(refresh_status, 300, NULL); /* the home screen is never deleted */
}
