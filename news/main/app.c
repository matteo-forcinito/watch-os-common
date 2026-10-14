/*
 * News, an app built into the OS (CONFIG_APPKIT_HOSTED).
 *
 * Boot:  news_service_start() loads the cached articles (PSRAM) and starts a
 *        1-minute timer: background rounds every few hours keep the archive
 *        growing for the week/month/year rankings (only with enough free RAM).
 * Open:  a round if the news is older than 20 minutes; the dashboard joins
 *        the OS web server at http://<watch>/news/ when its page is shown.
 * Close: a round in progress stops; nothing runs until the next timer slot.
 */
#include "news.h"

#include "appkit.h"
#include "core/app.h"
#include "core/sys.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ui/ui.h"

static const char *TAG = "news";
static bool s_ready;
static esp_timer_handle_t s_timer;

static void background_job(void *arg)
{
    (void)arg;
    nw_background_tick();
}

static void timer_cb(void *arg)
{
    (void)arg;
    sys_post(background_job, NULL);   /* esp_timer callbacks never do real work */
}

void news_service_start(void)
{
    s_ready = nw_start() == ESP_OK;
    if (!s_ready)
    {
        ESP_LOGE(TAG, "News not started (memory)");
        return;
    }
    const esp_timer_create_args_t args = {.callback = timer_cb, .name = "news"};
    if (esp_timer_create(&args, &s_timer) == ESP_OK)
    {
        esp_timer_start_periodic(s_timer, 60LL * 1000 * 1000);
    }
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    if (!s_ready)
    {
        ui_text(ui_page(screen, "Notizie"), "Memoria insufficiente all'avvio: riavvia l'orologio.", true);
        return;
    }
    nw_app_open(true);
    news_ui_create(screen);
    news_web_start();
}

static void destroy(void)
{
    if (!s_ready)
    {
        return;
    }
    appkit_web_stop();
    nw_app_open(false);
    news_ui_destroy();
}

const app_t news_app = {
    .id = "news",
    .name = "Notizie",
    .icon = LV_SYMBOL_FILE,
    .color = 0xFF6B5B,
    .create = create,
    .destroy = destroy,
    .back = news_ui_back,
    .about = "Il giornale sul polso: le storie raccontate da più testate, sezioni, classifiche del giorno, "
             "della settimana, del mese e dell'anno, cronologia e contesto da Wikipedia. Dashboard sul telefono.",
};
