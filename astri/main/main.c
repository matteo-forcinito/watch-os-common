/* Astri external app: same boot order as the other apps, Wi-Fi only for the clock (NTP). */
#include "astri.h"

#include "appkit.h"
#include "core/app.h"
#include "core/clock.h"
#include "core/devcon.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "esp_log.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "services/storage.h"
#include "services/time_sync.h"
#include "services/wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui/ui.h"

/* Opened from the OS the clock is already right (a software restart keeps it);
 * after a full reset it is not: NTP over Wi-Fi, a few seconds. */
static void clock_task(void *arg)
{
    (void)arg;
    if (time_sync_now() != ESP_OK)
        ESP_LOGW("astri", "Clock not synced: no known Wi-Fi network?");
    vTaskDelete(NULL);
}

static bool back(void)
{
    return astri_ui_back();
}

void app_main(void)
{
    ESP_ERROR_CHECK(extapp_sdk_init());
    sys_worker_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    esp_err_t storage = storage_init();
    if (storage != ESP_OK && storage != ESP_ERR_NOT_FOUND)
        ESP_LOGW("astri", "Flash storage unavailable: %s", esp_err_to_name(storage));
    ESP_ERROR_CHECK(lv_port_init());
    clock_init();
    appkit_alarms_prepare(); /* the OS alarms keep ringing in the app */
    ESP_ERROR_CHECK(wifi_service_init());
    if (time(NULL) < 1735689600)
        xTaskCreate(clock_task, "astri_ntp", 4096, NULL, 3, NULL);
    lv_port_lock();
    ui_init();
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, LV_PCT(100), LV_PCT(100));
    astri_ui_create(home);
    app_manager_set_home(home, back);
    lv_port_unlock();
    ESP_ERROR_CHECK(power_init());
    app_manager_init();
    appkit_alarms_start();
    devcon_start(); /* development console: QA builds only, a no-op otherwise */
    sys_heap_log("Astri ready");
}
