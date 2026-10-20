#include "casa.h"
#include "appkit.h"
#include "core/app.h"
#include "ui/ui.h"

#include <stdio.h>

/* Shell pages and intentional app pages share one live HA session.
 * Owners acquire/release only on the LVGL task. */
static unsigned s_owners;
static bool s_app_acquired;
static struct
{
    int view;
    char id[CASA_ID];
} s_request;

esp_err_t casa_acquire(void)
{
    if (!s_owners)
    {
        esp_err_t err = casa_start();
        if (err != ESP_OK) return err;
    }
    s_owners++;
    casa_web_start();
    return ESP_OK;
}

void casa_release(void)
{
    if (!s_owners || --s_owners) return;
    casa_web_stop();
    casa_stop();
}

void casa_open_view(int view, const char *id)
{
    /* App dispatch is queued; its argument must outlive this callback. */
    s_request.view = view;
    snprintf(s_request.id, sizeof(s_request.id), "%s", id ? id : "");
    app_open("casa", &s_request);
}

static void create(lv_obj_t *screen, void *arg)
{
    s_app_acquired = casa_acquire() == ESP_OK;
    if (!s_app_acquired)
    {
        ui_text(ui_page(screen, "Casa"), "Memoria insufficiente. Chiudi e riapri Casa.", true);
        return;
    }
    if (arg == &s_request) casa_ui_create_view(screen, s_request.view, s_request.id);
    else casa_ui_create(screen);
}

static void destroy(void)
{
    casa_ui_destroy();
    if (s_app_acquired) casa_release();
    s_app_acquired = false;
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
             "Scegli Casa come homepage nelle impostazioni Schermo.",
};
