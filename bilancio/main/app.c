/*
 * Bilancio as an app built into the OS (CONFIG_APPKIT_HOSTED). main.c is the
 * external app's entry instead; the rest of the code is the same.
 *
 * Open:  the book is loaded once (PSRAM) and stays loaded; the dashboard
 *        joins the OS web server at http://<watch>/bilancio/.
 * Close: pending changes are saved now.
 */
#include "bilancio.h"

#include "appkit.h"
#include "core/app.h"
#include "ui/ui.h"

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    if (bl_start() != ESP_OK)
    {
        ui_text(ui_page(screen, "Bilancio"), "Memoria insufficiente: chiudi e riapri l'app.", true);
        return;
    }
    bilancio_ui_create(screen);
    bilancio_web_start();
}

static void destroy(void)
{
    appkit_web_stop();
    bilancio_ui_destroy();
    bl_flush();
}

const app_t bilancio_app = {
    .id = "bilancio",
    .name = "Bilancio",
    .icon = LV_SYMBOL_HOME,
    .color = 0x14B87A,
    .create = create,
    .destroy = destroy,
    .back = bilancio_ui_back,
    .about = "Quanto puoi spendere oggi fino allo stipendio, cosa scade questo mese (mutuo, rate, "
             "bollette) e dove vanno i soldi. Dati sull'orologio, inserimento dalla dashboard.",
};
