/* Copertura external app: boot order as the other apps, then the probe, the dashboard and the UI. */
#include "copertura.h"

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
#include "services/wifi.h"
#include "ui/ui.h"

void app_main(void)
{
    ESP_ERROR_CHECK(extapp_sdk_init());
    sys_worker_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    /* SD-only boards legitimately have no FAT partition in internal flash. */
    esp_err_t storage = storage_init();
    if (storage != ESP_OK && storage != ESP_ERR_NOT_FOUND)
        ESP_LOGW("copertura", "Flash storage unavailable: %s", esp_err_to_name(storage));
    ESP_ERROR_CHECK(lv_port_init());
    clock_init();
    appkit_alarms_prepare(); /* the OS alarms keep ringing in the app */
    ESP_ERROR_CHECK(wifi_service_init());
    ESP_ERROR_CHECK(cp_start());
    lv_port_lock();
    ui_init();
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, LV_PCT(100), LV_PCT(100));
    copertura_ui_create(home);
    app_manager_set_home(home, copertura_ui_back);
    lv_port_unlock();
    ESP_ERROR_CHECK(power_init());
    app_manager_init();
    appkit_alarms_start();
    copertura_web_start();
    devcon_start(); /* development console: QA builds only, a no-op otherwise */
    sys_heap_log("Copertura ready");
}
