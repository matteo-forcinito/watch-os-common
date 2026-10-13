#include "core/sys.h"

#include "core/lv_port.h"

#include "hardware/pmu.h"

#if !CONFIG_ESP_COREDUMP_ENABLE_TO_NONE
#include "esp_core_dump.h"
#endif
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"

#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "sys";

#define WORKER_STACK  6144
#define WORKER_QUEUE  16

#define HEALTH_PERIOD_S     600   /* one health report every 10 minutes */
#define HEALTH_STACK_WARN   600   /* bytes of stack never used: less is a warning */

typedef struct
{
    sys_job_fn_t fn;
    void *arg;
} job_t;

static QueueHandle_t s_jobs = NULL;
static char s_reset_text[64] = "";
static bool s_crashed = false;

static void worker_task(void *arg)
{
    (void)arg;

    job_t job;

    while (true)
    {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE)
        {
            job.fn(job.arg);
        }
    }
}

void sys_worker_init(void)
{
    if (s_jobs != NULL)
    {
        return;
    }

    s_jobs = xQueueCreate(WORKER_QUEUE, sizeof(job_t));
    xTaskCreatePinnedToCore(worker_task, "sys", WORKER_STACK, NULL, 3, NULL, 0);
}

bool sys_post(sys_job_fn_t fn, void *arg)
{
    job_t job = {.fn = fn, .arg = arg};

    if (s_jobs == NULL || xQueueSend(s_jobs, &job, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "Worker queue full, job dropped");
        return false;
    }

    return true;
}

void sys_check_last_reset(void)
{
    esp_reset_reason_t reason = esp_reset_reason();

    switch (reason)
    {
        case ESP_RST_POWERON:   snprintf(s_reset_text, sizeof(s_reset_text), "Accensione"); break;
        case ESP_RST_SW:        snprintf(s_reset_text, sizeof(s_reset_text), "Riavvio"); break;
        case ESP_RST_USB:       snprintf(s_reset_text, sizeof(s_reset_text), "Reset USB"); break;
        case ESP_RST_BROWNOUT:  snprintf(s_reset_text, sizeof(s_reset_text), "Batteria scarica"); break;
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            s_crashed = true;
            break;
        default:                snprintf(s_reset_text, sizeof(s_reset_text), "Reset (%d)", (int)reason); break;
    }

    if (!s_crashed)
    {
        ESP_LOGI(TAG, "Start: %s", s_reset_text);
        return;
    }

    const char *kind = reason == ESP_RST_PANIC ? "crash" : "watchdog";
    #if !CONFIG_ESP_COREDUMP_ENABLE_TO_NONE
    esp_core_dump_summary_t *summary = calloc(1, sizeof(esp_core_dump_summary_t));

    if (summary != NULL && esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(summary) == ESP_OK)
    {
        snprintf(s_reset_text, sizeof(s_reset_text), "Errore (%s): %.15s @0x%08lx", kind, summary->exc_task,
                 (unsigned long)summary->exc_pc);
        ESP_LOGE(TAG, "PREVIOUS RUN CRASHED: %s. Details: idf.py coredump-info", s_reset_text);
    }
    else
    {
        snprintf(s_reset_text, sizeof(s_reset_text), "Errore (%s)", kind);
        ESP_LOGE(TAG, "PREVIOUS RUN CRASHED (%s), no core dump", kind);
    }

    free(summary);
    #else
    snprintf(s_reset_text, sizeof(s_reset_text), "Errore (%s)", kind);
    ESP_LOGE(TAG, "PREVIOUS RUN CRASHED (%s), core dump disabled", kind);
    #endif
}

const char *sys_last_reset_text(void)
{
    return s_reset_text;
}

bool sys_last_reset_was_crash(void)
{
    return s_crashed;
}

size_t sys_internal_free(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t sys_internal_largest(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t sys_internal_min_free(void)
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void sys_heap_log(const char *where)
{
    ESP_LOGI(TAG, "[%s] internal %u KB free (largest %u KB, min %u KB), PSRAM %u KB free", where,
             (unsigned)(sys_internal_free() / 1024), (unsigned)(sys_internal_largest() / 1024),
             (unsigned)(sys_internal_min_free() / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

bool sys_task_create(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t priority,
                     BaseType_t core)
{
    if (xTaskCreatePinnedToCore(fn, name, stack, arg, priority, NULL, core) == pdPASS)
    {
        return true;
    }

    ESP_LOGE(TAG, "Cannot start task \"%s\" (%u bytes of stack)", name, (unsigned)stack);
    sys_heap_log(name);

    return false;
}

/* ------------------------------------------------------------- health */

/*
 * One report in the log: RAM (now and the lowest ever), the slowest UI pass
 * and the stack each task never touched. Tasks close to overflowing are
 * reported as warnings, so a crash can be prevented before it happens.
 */
void sys_health_log(void)
{
    uint32_t max_pass_ms = 0;
    uint32_t flush_timeouts = 0;

    lv_port_take_stats(&max_pass_ms, &flush_timeouts);
    sys_heap_log("health");
    ESP_LOGI(TAG, "[health] up %lld min, slowest UI pass %u ms, lost flush IRQs %u",
             (long long)(esp_timer_get_time() / 60000000), (unsigned)max_pass_ms, (unsigned)flush_timeouts);

    if (pmu_present())
    {
        ESP_LOGI(TAG, "[health] battery %d%% %d mV%s%s", pmu_battery_percent(), pmu_battery_mv(),
                 pmu_is_charging() ? ", charging" : "", pmu_usb_present() ? ", USB" : "");
    }

#if configUSE_TRACE_FACILITY
    UBaseType_t room = uxTaskGetNumberOfTasks() + 4;   /* a few may start meanwhile */
    TaskStatus_t *tasks = malloc(sizeof(TaskStatus_t) * room);

    if (tasks == NULL)
    {
        return;
    }

    UBaseType_t count = uxTaskGetSystemState(tasks, room, NULL);
    char line[200];
    int used = 0;

    for (UBaseType_t i = 0; i < count; i++)
    {
        unsigned free_bytes = (unsigned)tasks[i].usStackHighWaterMark;   /* bytes on ESP-IDF */

        if (free_bytes < HEALTH_STACK_WARN)
        {
            ESP_LOGW(TAG, "[health] task \"%s\" has only %u bytes of stack left", tasks[i].pcTaskName, free_bytes);
        }

        used += snprintf(line + used, sizeof(line) - used, " %s:%u", tasks[i].pcTaskName, free_bytes);

        if (used > (int)sizeof(line) - 30 || i == count - 1)
        {
            ESP_LOGI(TAG, "[health] stack free:%s", line);
            used = 0;
        }
    }

    free(tasks);
#endif
}

static void health_job(void *arg)
{
    (void)arg;
    sys_health_log();
}

static void health_timer_cb(void *arg)
{
    (void)arg;
    sys_post(health_job, NULL);
}

void sys_health_start(void)
{
    static esp_timer_handle_t timer = NULL;

    if (timer != NULL)
    {
        return;
    }

    const esp_timer_create_args_t args = {
        .callback = health_timer_cb,
        .name = "health",
        .skip_unhandled_events = true,
    };

    if (esp_timer_create(&args, &timer) == ESP_OK)
    {
        esp_timer_start_periodic(timer, (uint64_t)HEALTH_PERIOD_S * 1000000);
    }
}
