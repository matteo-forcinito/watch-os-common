#include "extapp_sdk.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

static const char *TAG = "extapp_sdk";

esp_err_t extapp_sdk_init(void)
{
    const esp_partition_t *launcher = esp_ota_get_next_update_partition(NULL);

    if (launcher == NULL)
    {
        ESP_LOGW(TAG, "No launcher slot found");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_ota_set_boot_partition(launcher);

    ESP_LOGI(TAG, "Next boot: launcher in %s (%s)", launcher->label, esp_err_to_name(err));

    return err;
}

void extapp_return_to_launcher(void)
{
    extapp_sdk_init();
    esp_restart();
}

void extapp_restart_self(void)
{
    const esp_partition_t *self = esp_ota_get_running_partition();

    if (self != NULL)
    {
        esp_ota_set_boot_partition(self);   /* extapp_sdk_init() points back to the launcher at start */
    }

    esp_restart();
}
