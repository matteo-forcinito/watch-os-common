/* A transient worker owns the IMU; no I2C traffic in UI callbacks.
 * Closing/covering the app releases the sensor and this task's stack. */
#include "services/motion.h"
#include "core/sys.h"
#include "core/power.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "motion";
static portMUX_TYPE s_guard = portMUX_INITIALIZER_UNLOCKED;
static motion_snapshot_t s_state;
static bool s_busy;
static bool s_stop;

static void worker(void *arg)
{
    (void)arg;
    motion_snapshot_t state = {.running = true};
    state.error = imu_start();
    bool opened = state.error == ESP_OK;
    unsigned misses = 0;
    while (opened && !__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE) && state.error == ESP_OK &&
           power_screen() < POWER_SCREEN_AOD)
    {
        imu_sample_t sample;
        esp_err_t err = imu_read(&sample);
        if (err == ESP_OK)
        {
            state.sample = sample;
            state.valid = true;
            state.samples++;
            misses = 0;
        }
        else if (err != ESP_ERR_NOT_FINISHED || ++misses >= 20)
        {
            state.error = err == ESP_ERR_NOT_FINISHED ? ESP_ERR_TIMEOUT : err;
            state.valid = false;
        }
        portENTER_CRITICAL(&s_guard);
        s_state = state;
        portEXIT_CRITICAL(&s_guard);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (opened)
    {
        esp_err_t err = imu_stop();
        if (state.error == ESP_OK)
        {
            state.error = err;
        }
    }
    state.running = false;
    state.valid = false;
    portENTER_CRITICAL(&s_guard);
    s_state = state;
    portEXIT_CRITICAL(&s_guard);
    ESP_LOGI(TAG, "Stopped after %lu samples: %s", (unsigned long)state.samples, esp_err_to_name(state.error));
    __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
    vTaskDelete(NULL);
}
esp_err_t motion_start(void)
{
    if (!imu_supported())
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    bool expected = false;
    if (!__atomic_compare_exchange_n(&s_busy, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        return ESP_ERR_INVALID_STATE;
    }
    __atomic_store_n(&s_stop, false, __ATOMIC_RELEASE);
    portENTER_CRITICAL(&s_guard);
    s_state = (motion_snapshot_t){.running = true};
    portEXIT_CRITICAL(&s_guard);
    if (!sys_task_create(worker, "motion", 3072, NULL, 2, SYS_CORE_ANY))
    {
        portENTER_CRITICAL(&s_guard);
        s_state = (motion_snapshot_t){.error = ESP_ERR_NO_MEM};
        portEXIT_CRITICAL(&s_guard);
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void motion_stop(void)
{
    __atomic_store_n(&s_stop, true, __ATOMIC_RELEASE);
}
void motion_snapshot(motion_snapshot_t *out)
{
    if (out != NULL)
    {
        portENTER_CRITICAL(&s_guard);
        *out = s_state;
        portEXIT_CRITICAL(&s_guard);
    }
}
