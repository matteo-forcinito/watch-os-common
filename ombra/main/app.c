/*
 * Ombra as an app built into the OS (CONFIG_APPKIT_HOSTED). main.c is the
 * external app's entry instead; the rest of the code is the same.
 *
 * The OS Bluetooth (ble_companion) scans for Ombra while the app is open, so
 * there is one Bluetooth owner and the phone stays connected. Bluetooth off:
 * the app offers to switch it on.
 */
#include "ombra.h"

#include "core/app.h"
#include "core/settings.h"
#include "esp_log.h"
#include "ui/ui.h"

static const char *TAG = "ombra";

static void enable_bluetooth(lv_event_t *e)
{
    (void)e;
    settings_t s = *settings_get();
    s.ble_enabled = true;
    settings_save(&s);   /* the companion starts Bluetooth; reopen Ombra */
    app_back();
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    if (!ombra_bluetooth_on())
    {
        lv_obj_t *page = ui_page(screen, "Ombra");
        ui_text(page, "Ombra ascolta i segnali Bluetooth intorno a te: accendi il Bluetooth.", true);
        ui_button(page, "Accendi Bluetooth", 0xBF5AF2, enable_bluetooth, NULL);
        return;
    }
    if (ombra_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "Scanner not started");
        ui_text(ui_page(screen, "Ombra"), "Memoria insufficiente: chiudi e riapri l'app.", true);
        return;
    }
    ombra_sound_init();
    ombra_ui_create(screen);
}

static void destroy(void)
{
    ombra_stop();
    ombra_sound_stop();
    ombra_ui_destroy();
}

const app_t ombra_app = {
    .id = "ombra",
    .name = "Ombra",
    .icon = LV_SYMBOL_EYE_OPEN,
    .color = 0xBF5AF2,
    .create = create,
    .destroy = destroy,
    .back = ombra_ui_back,
    .about = "Mostra i dispositivi Bluetooth intorno a te, riconosce i tracker (AirTag, SmartTag...) "
             "e avvisa se uno ti segue. Il suono ti guida a trovarli.",
};
