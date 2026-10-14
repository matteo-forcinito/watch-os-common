/*
 * Pieno as an app built into the OS (CONFIG_APPKIT_HOSTED). main.c is the
 * external app's entry instead; the rest of the code is the same.
 *
 * Open:  the price worker starts (cached prices show at once) and the
 *        dashboard joins the OS web server at http://<watch>/pieno/.
 * Close: the worker ends after a download in progress, the dashboard folder
 *        answers "open the app". The price table stays in PSRAM.
 */
#include "pieno.h"

#include "appkit.h"
#include "core/app.h"
#include "esp_log.h"
#include "ui/ui.h"

static const char *TAG = "pieno";

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    if (pn_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "Price service not started");
        ui_text(ui_page(screen, "Pieno"), "Memoria insufficiente: chiudi e riapri l'app.", true);
        return;
    }
    pieno_ui_create(screen);
    pieno_web_start();
}

static void destroy(void)
{
    appkit_web_stop();
    pn_stop();
    pieno_ui_destroy();
}

const app_t pieno_app = {
    .id = "pieno",
    .name = "Pieno",
    .icon = LV_SYMBOL_GPS,
    .color = 0x30D158,
    .create = create,
    .destroy = destroy,
    .back = pieno_ui_back,
    .about = "Prezzi dei carburanti vicino a te (dati MIMIT): il distributore più conveniente, "
             "l'andamento della zona e una dashboard sul telefono.",
};
