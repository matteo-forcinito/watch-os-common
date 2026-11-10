#include "extapp_sdk.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "extapp_sdk";

/*
 * The launcher's slot. The OS writes its label to NVS ("extapp"/"launcher")
 * just before booting an app; with an app bay (slots beyond ota_1) the "other"
 * slot is not the launcher. Without the key: the classic two-slot rule.
 * NVS is shared with the OS: opened, never erased.
 */
static const esp_partition_t *launcher_slot(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("extapp", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_INITIALIZED && nvs_flash_init() == ESP_OK)
    {
        err = nvs_open("extapp", NVS_READONLY, &nvs);
    }
    if (err == ESP_OK)
    {
        char label[17] = "";
        size_t size = sizeof(label);
        err = nvs_get_str(nvs, "launcher", label, &size);
        nvs_close(nvs);
        const esp_partition_t *slot = err == ESP_OK ?
            esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label) : NULL;
        esp_app_desc_t desc;
        if (slot != NULL && slot != esp_ota_get_running_partition() &&
            esp_ota_get_partition_description(slot, &desc) == ESP_OK)   /* still holds an image */
        {
            return slot;
        }
    }
    return esp_ota_get_next_update_partition(NULL);
}

static bool s_is_app;

bool extapp_sdk_is_app(void)
{
    return s_is_app;
}

esp_err_t extapp_sdk_init(void)
{
    s_is_app = true;
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

    const esp_partition_t *launcher = launcher_slot();

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
