#include "core/sys.h"

#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/queue.h"

#include <stdio.h>

static const char *TAG = "sys";

#define WORKER_STACK  6144
#define WORKER_QUEUE  16

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
