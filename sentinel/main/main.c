#include "sentinel.h"
#include "core/app.h"
#include "core/clock.h"
#include "core/devcon.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "services/storage.h"
#include "services/wifi.h"
#include "ui/ui.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void app_main(void)
{
    ESP_ERROR_CHECK(extapp_sdk_init());
    sys_worker_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    /* SD-only boards legitimately have no FAT partition in internal flash. */
    esp_err_t storage = storage_init();
    if (storage != ESP_OK && storage != ESP_ERR_NOT_FOUND)
        ESP_LOGW("sentinel","Flash storage unavailable: %s",esp_err_to_name(storage));
    ESP_ERROR_CHECK(lv_port_init());
    clock_init();
    sn_alarm_prepare();
    ESP_ERROR_CHECK(wifi_service_init());
    ESP_ERROR_CHECK(sn_init());
    lv_port_lock();
    ui_init();
    lv_obj_t *home = lv_obj_create(NULL);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home,LV_PCT(100),LV_PCT(100));
    lv_obj_set_style_bg_color(home,ui_color(0x061A1B),0);
    lv_obj_set_style_bg_opa(home,LV_OPA_COVER,0);
    sn_ui_create(home);
    app_manager_set_home(home,sn_ui_back);
    lv_port_unlock();
    ESP_ERROR_CHECK(power_init());
    app_manager_init();
    sn_alarm_start();
    devcon_start();
    sys_heap_log("Sentinel ready");
}

#if CONFIG_WATCH_DEV_CONSOLE
#include "services/extapp.h"
#include "services/market.h"
bool devcon_app_command(char *line)
{
    if (strncmp(line,"net",3) != 0 || (line[3] && line[3] != ' ')) return false;
    strtok(line," ");
    char *operation = strtok(NULL," ");
    if (operation && strcmp(operation,"scan") == 0) printf("@NETREQUEST %s\n",esp_err_to_name(sn_scan(strtok(NULL," "))));
    else if (operation && strcmp(operation,"selftest") == 0) printf("@NETREQUEST %s\n",esp_err_to_name(sn_selftest()));
    else if (operation && strcmp(operation,"cancel") == 0) sn_cancel();
    else if (operation && strcmp(operation,"diag") == 0) printf("@NETREQUEST %s\n",esp_err_to_name(sn_diagnose(strtok(NULL," "))));
    else if (operation && strcmp(operation,"identify") == 0) printf("@NETREQUEST %s\n",esp_err_to_name(sn_identify(strtok(NULL," "))));
    else if (operation && strcmp(operation,"quality") == 0) printf("@NETREQUEST %s\n",esp_err_to_name(sn_quality_start(15)));
    else if (operation && strcmp(operation,"speed") == 0) printf("@NETREQUEST %s\n",esp_err_to_name(sn_speed_start(2,false)));
    else if (operation && strcmp(operation,"wifiscan") == 0) printf("@NETREQUEST %s\n",esp_err_to_name(sn_wifi_scan()));
    else if (operation && strcmp(operation,"join") == 0) printf("@NETREQUEST %s\n",esp_err_to_name(sn_wifi_join(strtok(NULL,""),NULL)));
    else if (operation && strcmp(operation,"report") == 0)
    {
        sn_status_t state;
        sn_quality_t q;
        if (sn_status(&state)) printf("@NETREPORT %s\n",state.diagnostic);
        printf("@NETTIME valid=%d epoch=%lld\n",clock_is_valid(),(long long)time(NULL));
        if (sn_quality(&q)) printf("@NETQUALITY %s | %s\n",q.verdict,q.speed_text);
        sn_ap_t *aps = malloc(sizeof(sn_ap_t)*SN_APS);
        int count = aps ? sn_wifi_list(aps,SN_APS) : 0;
        for (int i = 0; i < count; i++)
            printf("@NETAP %s ch=%d rssi=%d %s%s%s\n",aps[i].ssid,aps[i].channel,aps[i].rssi,aps[i].security,
                   aps[i].known ? " known" : "",aps[i].current ? " current" : "");
        free(aps);
    }
    else if (operation && strcmp(operation,"devices") == 0)
    {
        sn_status_t state;
        sn_device_t d;
        if (sn_status(&state))
            for (int i = 0; i < state.devices; i++)
            {
                if (!sn_device_at(i,&d) || strcmp(d.network,state.network) != 0) continue;
                char vendor[48], type[64], reasons[160];
                sn_vendor(&d,vendor,sizeof(vendor));
                sn_guess(&d,strcmp(d.ip,state.gateway) == 0,type,sizeof(type),reasons,sizeof(reasons));
                printf("@NETDEV %s %s | %s | mdns=%s dns=%s nb=%s | %s | %s | guess=%s (%s)\n",d.ip,d.mac,vendor,d.hostname,
                       d.dns,d.netbios,d.model,d.services,type,reasons);
                fflush(stdout);
            }
    }
    else if (operation && strcmp(operation,"cache-clear") == 0)
    {
        /* Manual QA flashing replaces OTA1 without the launcher's SD stamp. */
        extapp_forget_cache();
        printf("@NETCACHE cleared\n");
    }
    else if (operation && strcmp(operation,"marketfetch") == 0)
    {
        esp_err_t error = market_init();
        if (error == ESP_OK) error = market_refresh();
        printf("@NETMARKETREQUEST %s\n",esp_err_to_name(error));
    }
    else if (operation && strcmp(operation,"marketdownload") == 0)
    {
        /* Downloads files only; never call extapp_launch from an app slot. */
        printf("@NETMARKETREQUEST %s\n",esp_err_to_name(market_install("Sentinel")));
    }
    else if (operation && strcmp(operation,"marketstatus") == 0)
    {
        market_status_t state;
        market_status(&state);
        printf("@NETMARKET busy=%d phase=%d percent=%d message=%s\n",state.busy,state.phase,state.percent,state.message);
        for (int i = 0; i < market_count(); i++)
        {
            market_entry_t entry;
            if (market_entry(i,&entry)) printf("@NETPACKAGE %s %s installed=%d size=%u\n",entry.id,entry.version,entry.installed,(unsigned)entry.binary_size);
        }
    }
    else if (operation && strcmp(operation,"exit") == 0)
    {
        extapp_forget_cache();
        printf("@NETREQUEST %s\n",esp_err_to_name(sn_exit(false)));
    }
    sn_status_t status;
    if (sn_status(&status))
        printf("@NET busy=%d connected=%d ip=%s pair=%s devices=%d observed=%d new=%d progress=%d saved=%d dirty=%d msg=%s\n",
            status.busy,status.connected,status.ip,status.code,status.devices,status.observed,status.new_devices,
            status.percent,status.persistent,status.dirty,status.message);
    fflush(stdout);
    printf("@NETHEAP model=%u psram_free=%u psram_min=%u\n",(unsigned)sizeof(sn_model_t),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    fflush(stdout);
    return true;
}
#endif
