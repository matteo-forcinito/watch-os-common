/*
 * Giornata as an app built into the OS (CONFIG_APPKIT_HOSTED). main.c is the
 * external app's entry instead; the rest of the code is the same.
 *
 * Boot:  giornata_service_start() loads the agenda (PSRAM) and starts the
 *        2-second agenda tick (reminders, focus timer): reminders ring as OS
 *        notifications even with the app closed; calendars sync every 3 hours.
 * Open:  calendars sync now and every 20 minutes; the dashboard joins the OS
 *        web server at http://<watch>/giornata/ when its page is shown.
 */
#include "giornata.h"

#include "appkit.h"
#include "core/app.h"
#include "esp_log.h"
#include "ui/ui.h"

static const char *TAG = "giornata";
static bool s_ready;

void giornata_service_start(void)
{
    s_ready = gd_start() == ESP_OK;
    if (!s_ready)
    {
        ESP_LOGE(TAG, "Agenda not started (memory)");
    }
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    if (!s_ready)
    {
        ui_text(ui_page(screen, "Giornata"), "Memoria insufficiente all'avvio: riavvia l'orologio.", true);
        return;
    }
    gd_app_open(true);
    giornata_ui_create(screen);
    giornata_web_start();
}

static void destroy(void)
{
    if (!s_ready)
    {
        return;
    }
    appkit_web_stop();
    gd_app_open(false);
    giornata_ui_destroy();
}

const app_t giornata_app = {
    .id = "giornata",
    .name = "Giornata",
    .icon = LV_SYMBOL_LIST,
    .color = 0x5B8CFF,
    .create = create,
    .destroy = destroy,
    .back = giornata_ui_back,
    .about = "Agenda della giornata dai tuoi calendari (link ICS), attività e sessioni di focus, "
             "con promemoria anche ad app chiusa e una dashboard sul telefono.",
};
