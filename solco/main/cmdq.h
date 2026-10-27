#ifndef SOLCO_CMDQ_H
#define SOLCO_CMDQ_H
/* Bounded lock-free command queue: many producers (UI, storage, motion, console,
 * later the radio), one consumer (the audio task). Vyukov's per-cell sequence
 * scheme: a producer claims a cell with one compare-and-swap, the consumer never
 * waits, spins or takes a lock. Pure C11, also built by the host tests. */
#include "studio.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define SOLCO_CMDQ_SIZE 64 /* power of two */

typedef struct
{
    uint8_t type, deck;
    int32_t value;
    int64_t wide;
    studio_event_t event;
} solco_cmd_t;

typedef struct
{
    atomic_uint sequence;
    solco_cmd_t cmd;
} solco_cmdq_cell_t;

typedef struct
{
    solco_cmdq_cell_t cell[SOLCO_CMDQ_SIZE];
    atomic_uint head; /* next cell to claim by a producer */
    unsigned tail;    /* consumer only */
    atomic_uint dropped;
} solco_cmdq_t;

static inline void solco_cmdq_init(solco_cmdq_t *q)
{
    for (unsigned i = 0; i < SOLCO_CMDQ_SIZE; ++i)
        atomic_init(&q->cell[i].sequence, i);
    atomic_init(&q->head, 0);
    q->tail = 0;
    atomic_init(&q->dropped, 0);
}

/* Never blocks: false (and a drop count) when full. */
static inline bool solco_cmdq_push(solco_cmdq_t *q, const solco_cmd_t *cmd)
{
    unsigned pos = atomic_load_explicit(&q->head, memory_order_relaxed);
    for (;;)
    {
        solco_cmdq_cell_t *cell = &q->cell[pos & (SOLCO_CMDQ_SIZE - 1)];
        unsigned sequence = atomic_load_explicit(&cell->sequence, memory_order_acquire);
        int diff = (int)(sequence - pos);
        if (diff == 0)
        {
            if (atomic_compare_exchange_weak_explicit(&q->head, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed))
            {
                cell->cmd = *cmd;
                atomic_store_explicit(&cell->sequence, pos + 1, memory_order_release);
                return true;
            }
        }
        else if (diff < 0)
        {
            atomic_fetch_add_explicit(&q->dropped, 1, memory_order_relaxed);
            return false;
        }
        else
        {
            pos = atomic_load_explicit(&q->head, memory_order_relaxed);
        }
    }
}

/* Single consumer. */
static inline bool solco_cmdq_pop(solco_cmdq_t *q, solco_cmd_t *cmd)
{
    solco_cmdq_cell_t *cell = &q->cell[q->tail & (SOLCO_CMDQ_SIZE - 1)];
    unsigned sequence = atomic_load_explicit(&cell->sequence, memory_order_acquire);
    if ((int)(sequence - (q->tail + 1)) < 0)
        return false;
    *cmd = cell->cmd;
    atomic_store_explicit(&cell->sequence, q->tail + SOLCO_CMDQ_SIZE, memory_order_release);
    ++q->tail;
    return true;
}
#endif
