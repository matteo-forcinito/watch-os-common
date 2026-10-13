/* Ombra external app: boot order as the other apps, then Bluetooth and the UI. */
#include "ombra.h"

#include "core/app.h"
#include "core/devcon.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "esp_log.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "ui/ui.h"

void app_main(void)
{
    ESP_ERROR_CHECK(extapp_sdk_init());
    sys_worker_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(lv_port_init());
    esp_err_t error = ombra_start();
    if (error != ESP_OK)
        ESP_LOGE("ombra", "Bluetooth unavailable: %s", esp_err_to_name(error));
    ombra_sound_init();
    lv_port_lock();
    ui_init();
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, LV_PCT(100), LV_PCT(100));
    ombra_ui_create(home);
    app_manager_set_home(home, ombra_ui_back);
    lv_port_unlock();
    ESP_ERROR_CHECK(power_init());
    app_manager_init();
    devcon_start(); /* development console: QA builds only, a no-op otherwise */
    sys_heap_log("Ombra ready");
}
