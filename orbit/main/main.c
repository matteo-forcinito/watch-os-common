#include "orbit.h"
#include "core/app.h"
#include "core/clock.h"
#include "core/devcon.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "ui/ui.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_ERROR_CHECK(extapp_sdk_init());
    sys_worker_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(lv_port_init());
    clock_init();
    orbit_alarm_prepare();
    lv_port_lock();
    ui_init();
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(home, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    orbit_create(home);
    app_manager_set_home(home, orbit_back);
    lv_port_unlock();
    app_manager_init();
    ESP_ERROR_CHECK(power_init());
    orbit_alarm_start();
    devcon_start();
    sys_heap_log("ORBIT ready");
}
