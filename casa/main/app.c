#include "casa.h"
#include "appkit.h"
#include "core/app.h"
#include "ui/ui.h"

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    if (casa_start() != ESP_OK)
    {
        ui_text(ui_page(screen, "Casa"), "Memoria insufficiente. Chiudi e riapri Casa.", true);
        return;
    }
    casa_web_start();
    casa_ui_create(screen);
}

static void destroy(void)
{
    appkit_web_stop();
    casa_ui_destroy();
    casa_stop();
}

const app_t casa_app = {
    .id = "casa",
    .name = "Casa",
    .icon = LV_SYMBOL_HOME,
    .color = 0xFFC870,
    .create = create,
    .destroy = destroy,
    .back = casa_ui_back,
    .about = "Home Assistant locale o via Nabu Casa: stanze, tipologie, dashboard e controlli in tempo reale. "
             "Tieni premuta l'ora sulla homepage per aprire Casa.",
};
