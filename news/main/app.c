/*
 * News, an app built into the OS (CONFIG_APPKIT_HOSTED).
 *
 * Boot:  no archive allocation, task, timer or automatic download.
 * First explicit open: a worker loads the cached articles into PSRAM.
 * Open:  a round if the news is older than 20 minutes; the dashboard joins
 *        the OS web server at http://<watch>/news/ when its page is shown.
 * Close: a round in progress stops; no periodic background downloads.
 */
#include "news.h"

#include "appkit.h"
#include "core/app.h"
#include "core/sys.h"
#include "core/lv_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ui/ui.h"
#include <stdatomic.h>

static const char *TAG = "news";
static atomic_bool s_ready, s_loading;
static lv_obj_t *s_screen, *s_loading_page;
static bool s_mounted;

static void show_ready(void *arg)
{
    (void)arg;
    if (s_screen == NULL || s_mounted) return;
    if (s_loading_page != NULL) { lv_obj_delete(s_loading_page); s_loading_page = NULL; }
    if (!s_ready) {
        ui_text(ui_page(s_screen, "Notizie"), "Memoria insufficiente. Chiudi e riprova.", true);
        return;
    }
    s_mounted = true;
    nw_app_open(true);
    news_ui_create(s_screen);
    news_web_start();
}

static void load_task(void *arg)
{
    (void)arg;
    s_ready = nw_start() == ESP_OK;
    s_loading = false;
    if (!ui_async(show_ready, NULL)) ESP_LOGW(TAG, "UI queue busy; reopen News when ready");
    vTaskDelete(NULL);
}

void news_service_start(void)
{
    if (s_ready || atomic_exchange(&s_loading, true)) return;
    if (!sys_task_create(load_task, "news_init", 6144, NULL, 2, SYS_CORE_ANY)) s_loading = false;
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    s_screen = screen;
    if (s_ready) show_ready(NULL);
    else {
        s_loading_page = ui_page(screen, "Notizie");
        ui_text(s_loading_page, "Caricamento archivio...", true);
        news_service_start();
    }
}

static void destroy(void)
{
    s_screen = s_loading_page = NULL;
    if (!s_mounted) return;
    s_mounted = false;
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
