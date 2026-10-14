/*
 * Deck runner: one task executes actions and dial turns in order, so a macro
 * with waits never blocks the UI and two quick taps never interleave.
 */
#include "deck.h"
#include "action.h"

#include "core/sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hid_link/hid_link.h"

#include <string.h>

static const char *TAG = "deck_run";

typedef struct
{
    bool dial;
    int8_t delta;
    uint8_t dial_kind;
    char action[160];
} job_t;

static QueueHandle_t s_jobs;

static void run_action(const char *text)
{
    deck_action_t action;
    if (!deck_action_parse(text, &action))
    {
        ESP_LOGW(TAG, "Invalid action at %d: %s", action.error_at, action.error);
        return;
    }
    for (int i = 0; i < action.count; i++)
    {
        const deck_step_t *s = &action.steps[i];
        switch (s->kind)
        {
        case DECK_STEP_KEY:
            hid_link_key(s->modifiers, s->key);
            break;
        case DECK_STEP_TEXT:
        {
            char buffer[161];
            size_t n = s->length < sizeof(buffer) - 1 ? s->length : sizeof(buffer) - 1;
            memcpy(buffer, text + s->offset, n);
            buffer[n] = 0;
            hid_link_type(buffer);
            break;
        }
        case DECK_STEP_MEDIA:
            hid_link_media(s->value);
            break;
        case DECK_STEP_WHEEL:
            hid_link_mouse(0, 0, 0, s->wheel);
            break;
        case DECK_STEP_WAIT:
            vTaskDelay(pdMS_TO_TICKS(s->value));
            break;
        }
        /* A short gap between steps: some apps miss shortcuts sent back to back. */
        if (i + 1 < action.count && s->kind != DECK_STEP_WAIT)
            vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* One detent of the dial. Zoom uses the keypad +/- (same on every keyboard layout). */
static void run_dial(deck_dial_t dial, int delta)
{
    bool up = delta > 0;
    for (int n = delta > 0 ? delta : -delta; n > 0; n--)
    {
        switch (dial)
        {
        case DECK_DIAL_VOLUME: hid_link_media(up ? 0x00E9 : 0x00EA); break;
        case DECK_DIAL_SCROLL: hid_link_mouse(0, 0, 0, up ? -1 : 1); break;
        case DECK_DIAL_ZOOM: hid_link_key(0x01, up ? 0x57 : 0x56); break;
        case DECK_DIAL_TABS: hid_link_key(up ? 0x01 : 0x03, 0x2B); break;
        case DECK_DIAL_UNDO: hid_link_key(0x01, up ? 0x1C : 0x1D); break; /* ctrl+y / ctrl+z */
        case DECK_DIAL_ARROWS: hid_link_key(0, up ? 0x4F : 0x50); break;
        case DECK_DIAL_ARROWS_VERTICAL: hid_link_key(0, up ? 0x51 : 0x52); break;
        default: break;
        }
        vTaskDelay(pdMS_TO_TICKS(12));
    }
}

static void run_task(void *arg)
{
    (void)arg;
    job_t job;
    for (;;)
    {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE)
            continue;
        if (job.dial)
            run_dial((deck_dial_t)job.dial_kind, job.delta);
        else
            run_action(job.action);
    }
}

void deck_run_start(void)
{
    s_jobs = xQueueCreate(8, sizeof(job_t));
    sys_task_create(run_task, "deck_run", 4096, NULL, 5, tskNO_AFFINITY);
}

bool deck_connected(void) { return hid_link_state() == HID_LINK_CONNECTED; }

bool deck_run(const char *action)
{
    job_t job = {.dial = false};
    strncpy(job.action, action, sizeof(job.action) - 1);
    return s_jobs && xQueueSend(s_jobs, &job, 0) == pdTRUE;
}

void deck_dial(deck_dial_t dial, int delta)
{
    if (delta == 0 || s_jobs == NULL)
        return;
    job_t job = {.dial = true, .dial_kind = (uint8_t)dial, .delta = (int8_t)(delta > 20 ? 20 : delta < -20 ? -20 : delta)};
    xQueueSend(s_jobs, &job, 0);
}
