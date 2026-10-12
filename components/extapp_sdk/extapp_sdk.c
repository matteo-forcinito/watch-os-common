#include "extapp_sdk.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

static const char *TAG = "extapp_sdk";

esp_err_t extapp_sdk_init(void)
{
    /* Confirm the running app before selecting the launcher. With rollback
     * enabled, selecting another slot first loses the pending verification
     * entry, and restarting this app can mark it aborted. The launcher is
     * still selected below, so a later app crash returns to the main OS. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY)
    {
        esp_err_t validation = esp_ota_mark_app_valid_cancel_rollback();
        if (validation != ESP_OK)
        {
            ESP_LOGE(TAG, "App slot validation failed: %s", esp_err_to_name(validation));
            return validation;
        }
    }

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
