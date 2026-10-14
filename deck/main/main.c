/*
 * Deck external app: a programmable macro pad (USB or Bluetooth keyboard).
 * Two modes, chosen at start: keys (HID stacks) or configuration (Wi-Fi web
 * editor). Switching restarts the app: HID stacks are never torn down live.
 */
#include "deck.h"

#include "appkit.h"
#include "core/app.h"
#include "core/clock.h"
#include "core/devcon.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "esp_log.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "hid_link/hid_link.h"
#include "services/storage.h"
#include "services/wifi.h"
#include "ui/ui.h"

#include <stdio.h>

void deck_ui_create(lv_obj_t *screen, bool config_mode);
bool deck_ui_back(void);

void app_main(void)
{
    ESP_ERROR_CHECK(extapp_sdk_init());
    sys_worker_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    esp_err_t storage = storage_init();
    if (storage != ESP_OK && storage != ESP_ERR_NOT_FOUND)
        ESP_LOGW("deck", "Flash storage unavailable: %s", esp_err_to_name(storage));
    ESP_ERROR_CHECK(lv_port_init());
    clock_init();
    appkit_alarms_prepare(); /* the OS alarms keep ringing in the app */
    deck_profiles_load();

    /* Configuration mode lasts one start: leaving it (or a crash) gives the keys back. */
    deck_settings_t settings = *deck_settings();
    bool config_mode = settings.config_mode != 0;
    if (settings.transport == HID_LINK_USB && !board_info()->has_usb_otg)
        settings.transport = HID_LINK_BLE;
    if (config_mode)
    {
        settings.config_mode = 0;
        deck_settings_save(&settings);
        ESP_ERROR_CHECK(wifi_service_init());
        esp_err_t error = deck_web_start();
        if (error != ESP_OK)
            ESP_LOGE("deck", "Editor: %s", esp_err_to_name(error));
    }
    else
    {
        char name[32];
        snprintf(name, sizeof(name), "%.20s Deck", board_info()->short_name);
        esp_err_t error = hid_link_start((hid_link_transport_t)settings.transport, name);
        if (error != ESP_OK)
            ESP_LOGW("deck", "Connection start: %s", esp_err_to_name(error));
        deck_run_start();
    }

    lv_port_lock();
    ui_init();
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, LV_PCT(100), LV_PCT(100));
    deck_ui_create(home, config_mode);
    app_manager_set_home(home, deck_ui_back);
    lv_port_unlock();
    ESP_ERROR_CHECK(power_init());
    app_manager_init();
    appkit_alarms_start();
    if (config_mode || hid_link_transport() != HID_LINK_USB)
        devcon_start(); /* QA builds only; the USB port belongs to the keyboard in USB mode */
    sys_heap_log("Deck ready");
}
