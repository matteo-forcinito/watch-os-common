/* Anello external app: boot order as the other apps, then the game screen. */
#include "appkit.h"
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

void anello_start(lv_obj_t *screen);

void app_main(void)
{
    ESP_ERROR_CHECK(extapp_sdk_init());
    sys_worker_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(lv_port_init());
    clock_init();
    appkit_alarms_prepare(); /* the OS alarms keep ringing in the app */
    lv_port_lock();
    ui_init();
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, LV_PCT(100), LV_PCT(100));
    anello_start(home); /* also registers the back handler */
    lv_port_unlock();
    ESP_ERROR_CHECK(power_init());
    app_manager_init();
    appkit_alarms_start();
    devcon_start(); /* development console: QA builds only, a no-op otherwise */
    sys_heap_log("Anello ready");
}
