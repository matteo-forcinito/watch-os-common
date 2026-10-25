#include "core/power.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/sys.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "hardware/audio.h"
#include "hardware/board.h"
#include "lite.h"
#include "nvs.h"
#include "services/notify.h"
#include "services/sound.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

QueueHandle_t lite_events;
lite_settings_t lite_settings = {.version = 1, .aod = false, .brightness = 60, .web_minutes = 10};
typedef struct
{
    sys_job_fn_t fn;
    void *arg;
} job_t;
static QueueHandle_t jobs;
static atomic_int values[STATE_COUNT], sound_mode;
static atomic_bool sound_running, ringing;
static SemaphoreHandle_t sound_guard;
static portMUX_TYPE alert_lock = portMUX_INITIALIZER_UNLOCKED;
static char alert_text[80];
static uint32_t alarm_id;
static int64_t alert_until;
void lite_post(int kind, int value, int second)
{
    lite_event_t e = {kind, value, second};
    if (lite_events)
        xQueueSend(lite_events, &e, 0);
}
void state_set(state_id_t id, int32_t v)
{
    if (id < STATE_COUNT && atomic_exchange(&values[id], v) != v)
        lite_post(LITE_DIRTY, 0, 0);
}
void state_bump(state_id_t id)
{
    if (id < STATE_COUNT)
        atomic_fetch_add(&values[id], 1);
    lite_post(LITE_DIRTY, 0, 0);
}
int32_t state_get(state_id_t id)
{
    return id < STATE_COUNT ? atomic_load(&values[id]) : 0;
}
void power_wake(power_wake_t reason)
{
    (void)reason;
    lite_post(LITE_WAKE, 0, 0);
}
static void worker(void *arg)
{
    (void)arg;
    job_t job;
    for (;;)
        if (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE)
            job.fn(job.arg);
}
void sys_worker_init(void)
{
    if (jobs)
        return;
    jobs = xQueueCreate(16, sizeof(job_t));
    if (jobs)
        sys_task_create(worker, "lite_sys", 6144, NULL, 3, 0);
}
bool sys_post(sys_job_fn_t fn, void *arg)
{
    job_t j = {fn, arg};
    return jobs && xQueueSend(jobs, &j, 0) == pdTRUE;
}
bool sys_task_create(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t priority,
                     BaseType_t core)
{
    bool ok = xTaskCreatePinnedToCore(fn, name, stack, arg, priority, NULL, core) == pdPASS;
    ESP_LOGI("lite", "Task %s: %u B, %s", name, (unsigned)stack, ok ? "started" : "no memory");
    return ok;
}
const char *sys_nickname(void)
{
    return "Lite";
}
const char *sys_hostname(void)
{
    return "watch-lite";
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

static void speaker(void *arg)
{
    (void)arg;
    int16_t samples[256];
    unsigned phase = 0, elapsed = 0;
    bool opened = audio_start(16000, 1) == ESP_OK;
    if (opened)
        audio_set_volume(settings_get()->volume);
    for (;;)
    {
        if (!opened || !atomic_load(&sound_mode)) {
            xSemaphoreTake(sound_guard,portMAX_DELAY);
            if (opened && atomic_load(&sound_mode)) { xSemaphoreGive(sound_guard); continue; }
            audio_stop();
            atomic_store(&sound_mode,0);
            atomic_store(&sound_running,false);
            xSemaphoreGive(sound_guard);
            break;
        }
        int mode = atomic_load(&sound_mode);
        bool tone = (elapsed % 1000) < 550;
        for (unsigned i = 0; i < 256; i++)
        {
            phase++;
            samples[i] = tone ? ((phase % (elapsed % 1000 < 250 ? 27 : 21) < 13) ? 4500 : -4500) : 0;
        }
        if (audio_write(samples, 256) != ESP_OK) { opened=false; continue; }
        elapsed += 16;
        if (mode == 1 && elapsed >= 800) {
            xSemaphoreTake(sound_guard,portMAX_DELAY);
            if (atomic_load(&sound_mode)==1) atomic_store(&sound_mode,0);
            xSemaphoreGive(sound_guard);
        }
    }
    vTaskDelete(NULL);
}
static void play(int mode)
{
    if (!board_info()->has_audio || settings_get()->volume == 0)
        return;
    xSemaphoreTake(sound_guard,portMAX_DELAY);
    if (mode == 1 && atomic_load(&sound_mode) == 2) {
        xSemaphoreGive(sound_guard);
        return;
    }
    atomic_store(&sound_mode, mode);
    if (!atomic_exchange(&sound_running, true) && !sys_task_create(speaker, "lite_sound", 4096, NULL, 4, 1))
    {
        atomic_store(&sound_running, false);
        atomic_store(&sound_mode, 0);
    }
    xSemaphoreGive(sound_guard);
}
void sound_notification(void)
{
    play(1);
}
void sound_alarm_start(void)
{
    play(2);
}
void sound_alarm_stop(void)
{
    atomic_store(&sound_mode, 0);
}
bool sound_alarm_playing(void)
{
    return atomic_load(&sound_mode) == 2;
}
void sound_click(void)
{
}
void sound_service_init(void)
{
}
void lite_ring(const alarm_t *a)
{
    portENTER_CRITICAL(&alert_lock);
    alarm_id = a->id;
    alert_until = 0;
    snprintf(alert_text, sizeof(alert_text), "%s", a->label[0] ? a->label : "SVEGLIA");
    portEXIT_CRITICAL(&alert_lock);
    atomic_store(&ringing, true);
    sound_alarm_start();
    lite_post(LITE_WAKE, 0, 0);
}
void notify_add(const notify_t *n)
{
    /* An event must not replace an alarm already ringing. */
    if (atomic_load(&ringing) && alarm_id)
        return;
    portENTER_CRITICAL(&alert_lock);
    alarm_id = 0;
    alert_until = esp_timer_get_time() + 15000000;
    snprintf(alert_text, sizeof(alert_text), "%.23s: %.47s", n->app, n->title);
    portEXIT_CRITICAL(&alert_lock);
    atomic_store(&ringing, true);
    sound_notification();
    lite_post(LITE_WAKE, 0, 0);
}
bool lite_ringing(void)
{
    if (atomic_load(&ringing) && alert_until && esp_timer_get_time() > alert_until)
        atomic_store(&ringing, false);
    return atomic_load(&ringing);
}
void lite_alert_text(char *out, unsigned size)
{
    portENTER_CRITICAL(&alert_lock);
    snprintf(out, size, "%s", alert_text);
    portEXIT_CRITICAL(&alert_lock);
}
void lite_alert_stop(bool snooze)
{
    uint32_t id = alarm_id;
    atomic_store(&ringing, false);
    sound_alarm_stop();
    if (id)
    {
        if (snooze)
            alarm_snooze(id, 5);
        else
            alarm_dismiss(id);
    }
    alarm_id = 0;
    lite_post(LITE_DIRTY, 0, 0);
}
void lite_save(void)
{
    nvs_handle_t h;
    if (nvs_open("watch_lite", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_blob(h, "settings", &lite_settings, sizeof(lite_settings));
        nvs_commit(h);
        nvs_close(h);
    }
    lite_post(LITE_DIRTY, 0, 0);
}
void lite_runtime_init(void)
{
    sound_guard=xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(sound_guard?ESP_OK:ESP_ERR_NO_MEM);
    lite_events = xQueueCreate(24, sizeof(lite_event_t));
    ESP_ERROR_CHECK(lite_events ? ESP_OK : ESP_ERR_NO_MEM);
    sys_worker_init();
    nvs_handle_t h;
    if (nvs_open("watch_lite", NVS_READONLY, &h) == ESP_OK)
    {
        size_t n = sizeof(lite_settings);
        lite_settings_t saved;
        if (nvs_get_blob(h, "settings", &saved, &n) == ESP_OK && n == sizeof(saved) && saved.version == 1 &&
            saved.brightness >= 10 && saved.brightness <= 120 && saved.web_minutes >= 1 && saved.web_minutes <= 120)
            lite_settings = saved;
        nvs_close(h);
    }
}
