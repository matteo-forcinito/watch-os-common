#include "services/notify.h"

#include "core/power.h"
#include "core/settings.h"
#include "core/state.h"
#include "services/sound.h"

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

static EXT_RAM_BSS_ATTR notify_t s_items[NOTIFY_MAX];   /* 5.6 KB: in PSRAM */
static int s_count = 0;
static notify_call_t s_call;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void publish(void)
{
    state_set(STATE_NOTIF_COUNT, s_count);
    state_bump(STATE_NOTIF_VERSION);
}

void notify_init(void)
{
    s_count = 0;
    memset(&s_call, 0, sizeof(s_call));
}

void notify_add(const notify_t *n)
{
    portENTER_CRITICAL(&s_lock);

    /* Same id: the phone updated it (e.g. a chat with a new message). */
    for (int i = 0; i < s_count; i++)
    {
        if (s_items[i].id == n->id)
        {
            memmove(&s_items[i], &s_items[i + 1], sizeof(notify_t) * (size_t)(s_count - i - 1));
            s_count--;
            break;
        }
    }

    if (s_count == NOTIFY_MAX)
    {
        s_count--;   /* drop the oldest */
    }

    memmove(&s_items[1], &s_items[0], sizeof(notify_t) * (size_t)s_count);
    s_items[0] = *n;

    if (s_items[0].time == 0)
    {
        s_items[0].time = time(NULL);
    }

    s_count++;
    portEXIT_CRITICAL(&s_lock);

    publish();
    sound_notification();

    if (settings_get()->wake_on_notification)
    {
        power_wake(POWER_WAKE_NOTIFICATION);
    }
}

void notify_remove(uint32_t id)
{
    bool changed = false;

    portENTER_CRITICAL(&s_lock);

    for (int i = 0; i < s_count; i++)
    {
        if (s_items[i].id == id)
        {
            memmove(&s_items[i], &s_items[i + 1], sizeof(notify_t) * (size_t)(s_count - i - 1));
            s_count--;
            changed = true;
            break;
        }
    }

    portEXIT_CRITICAL(&s_lock);

    if (changed)
    {
        publish();
    }
}

void notify_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    s_count = 0;
    portEXIT_CRITICAL(&s_lock);

    publish();
}

int notify_count(void)
{
    return s_count;
}

bool notify_get(int index, notify_t *out)
{
    bool ok = false;

    portENTER_CRITICAL(&s_lock);

    if (index >= 0 && index < s_count)
    {
        *out = s_items[index];
        ok = true;
    }

    portEXIT_CRITICAL(&s_lock);

    return ok;
}

void notify_set_call(const notify_call_t *call)
{
    portENTER_CRITICAL(&s_lock);
    s_call = *call;
    portEXIT_CRITICAL(&s_lock);

    state_bump(STATE_NOTIF_VERSION);

    if (call->active)
    {
        power_wake(POWER_WAKE_NOTIFICATION);
    }
}

bool notify_get_call(notify_call_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_call;
    portEXIT_CRITICAL(&s_lock);

    return out->active;
}
