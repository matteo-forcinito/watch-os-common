/*
 * Copertura as an app built into the OS (CONFIG_APPKIT_HOSTED). main.c is the
 * external app's entry instead; the rest of the code is the same.
 *
 * Open:  the probe joins Wi-Fi (power save off) and samples every 0.5 s; the
 *        dashboard joins the OS web server at http://<watch>/copertura/ when
 *        its page is shown.
 * Close: the probe and the sound task end and Wi-Fi is released; the
 *        measured points stay in PSRAM (and on the card).
 */
#include "copertura.h"

#include "appkit.h"
#include "core/app.h"
#include "esp_log.h"
#include "ui/ui.h"

static const char *TAG = "copertura";

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    if (cp_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "Probe not started");
        ui_text(ui_page(screen, "Copertura"), "Memoria insufficiente: chiudi e riapri l'app.", true);
        return;
    }
    copertura_ui_create(screen);
    copertura_web_start();
}

static void destroy(void)
{
    appkit_web_stop();
    copertura_ui_destroy();
    cp_stop();
}

const app_t copertura_app = {
    .id = "copertura",
    .name = "Copertura",
    .icon = LV_SYMBOL_WIFI,
    .color = 0x0A84FF,
    .create = create,
    .destroy = destroy,
    .back = copertura_ui_back,
    .about = "Misura il Wi-Fi stanza per stanza (segnale, ping, perdite), trova il punto migliore "
             "con il suono e consiglia dove mettere il router. Mappa nella dashboard.",
};
