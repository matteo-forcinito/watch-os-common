#include "core/recovery.h"
#include "core/sys.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdint.h>
static const char *TAG = "recovery";
static bool s_active;
static bool s_supported;
static atomic_bool s_pending;
void recovery_init(void)
{
    s_supported = true;
    nvs_handle_t handle;
    uint8_t value = 0;
    esp_err_t err = nvs_open("recovery", NVS_READONLY, &handle);
    if (err == ESP_OK)
    {
        err = nvs_get_u8(handle, "enabled", &value);
        nvs_close(handle);
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "Cannot read mode: %s", esp_err_to_name(err));
    }
    s_active = value == 1;
}
bool recovery_supported(void)
{
    return s_supported;
}
bool recovery_active(void)
{
    return s_active;
}
esp_err_t recovery_set(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("recovery", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "enabled", enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
static void restart_job(void *arg)
{
    bool enabled = (intptr_t)arg != 0;
    esp_err_t err = recovery_set(enabled);
    if (err == ESP_OK)
    {
        ESP_LOGW(TAG, "Recovery %s; restarting", enabled ? "enabled" : "disabled");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    ESP_LOGE(TAG, "Mode not saved; staying here: %s", esp_err_to_name(err));
    atomic_store(&s_pending, false);
}
bool recovery_request(bool enabled)
{
    if (!s_supported) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_pending, &expected, true)) return false;
    if (!sys_post(restart_job, (void *)(intptr_t)enabled))
    {
        atomic_store(&s_pending, false);
        return false;
    }
    return true;
}
