#include "core/state.h"

#include "core/lv_port.h"

#include "freertos/FreeRTOS.h"

#include <stdatomic.h>

static lv_subject_t *s_subjects[STATE_COUNT];
static _Atomic int32_t s_values[STATE_COUNT];
static _Atomic uint32_t s_dirty = 0;
static bool s_ready = false;

_Static_assert(STATE_COUNT <= 32, "s_dirty is a 32-bit mask");

void state_init(void)
{
    for (int i = 0; i < STATE_COUNT; i++)
    {
        atomic_store(&s_values[i], 0);
        s_subjects[i] = lv_subject_create(LV_SUBJECT_TYPE_INT);
    }

    atomic_store(&s_values[STATE_BATTERY], -1);
    lv_subject_set_int(s_subjects[STATE_BATTERY], -1);
    s_ready = true;
}

void state_set(state_id_t id, int32_t value)
{
    if (id >= STATE_COUNT)
    {
        return;
    }

    if (atomic_exchange(&s_values[id], value) == value)
    {
        return;
    }

    atomic_fetch_or(&s_dirty, 1u << id);
    lv_port_wake();
}

void state_bump(state_id_t id)
{
    if (id >= STATE_COUNT)
    {
        return;
    }

    atomic_fetch_add(&s_values[id], 1);
    atomic_fetch_or(&s_dirty, 1u << id);
    lv_port_wake();
}

int32_t state_get(state_id_t id)
{
    return id < STATE_COUNT ? atomic_load(&s_values[id]) : 0;
}

lv_subject_t *state_subject(state_id_t id)
{
    return s_subjects[id];
}

void state_apply_pending(void)
{
    if (!s_ready)
    {
        return;
    }

    uint32_t dirty = atomic_exchange(&s_dirty, 0);

    for (int i = 0; dirty != 0 && i < STATE_COUNT; i++)
    {
        if (dirty & (1u << i))
        {
            dirty &= ~(1u << i);
            lv_subject_set_int(s_subjects[i], atomic_load(&s_values[i]));
        }
    }
}
