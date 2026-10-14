/*
 * Sentinel Lite, an app built into the OS (CONFIG_APPKIT_HOSTED).
 *
 * Open:  the tables (PSRAM) at the first open, a scan if the last one is older
 *        than 10 minutes; the dashboard at http://<watch>/sentinel/.
 * Close: a job in progress stops; Wi-Fi is released by each job.
 */
#include "lite.h"

#include "appkit.h"
#include "core/app.h"
#include "ui/ui.h"

static bool s_ready;

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    s_ready = sl_start() == ESP_OK;
    if (!s_ready)
    {
        ui_text(ui_page(screen, "Sentinel"), "Memoria insufficiente: chiudi e riapri l'app.", true);
        return;
    }
    sl_app_open(true);
    sentinel_ui_create(screen);
    sentinel_web_start();
}

static void destroy(void)
{
    if (!s_ready)
    {
        return;
    }
    appkit_web_stop();
    sl_app_open(false);
    sentinel_ui_destroy();
}

const app_t sentinel_app = {
    .id = "sentinel",
    .name = "Sentinel",
    .icon = LV_SYMBOL_EYE_OPEN,
    .color = 0x49E2BC,
    .create = create,
    .destroy = destroy,
    .back = sentinel_ui_back,
    .about = "Sentinel Lite: chi è collegato alla tua rete (con i nuovi arrivati), verifica di router e internet, "
             "reti vicine e servizi aperti dei dispositivi. Dashboard sul telefono.",
};
