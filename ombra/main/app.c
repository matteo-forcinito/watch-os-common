/*
 * Ombra as an app built into the OS (CONFIG_APPKIT_HOSTED). main.c is the
 * external app's entry instead; the rest of the code is the same.
 *
 * The OS Bluetooth (ble_companion) scans for Ombra while the app is open, so
 * there is one Bluetooth owner and the phone stays connected. Bluetooth off in
 * the settings: it is on only while Ombra is open.
 */
#include "ombra.h"

#include "core/app.h"
#include "companion/ble_companion.h"
#include "esp_log.h"
#include "ui/ui.h"

static const char *TAG = "ombra";

static bool s_holding;

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    /* Bluetooth off in the settings: on for this session only, off again at close. */
    ble_companion_hold(true);
    s_holding = true;
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
    if (s_holding)
    {
        s_holding = false;
        ble_companion_hold(false);
    }
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
