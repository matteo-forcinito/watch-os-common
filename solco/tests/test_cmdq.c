/* Lock-free command queue: order, full/empty bounds, and four concurrent producers
 * against one consumer with no lost or duplicated command. */
#include "cmdq.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

#define PRODUCERS 4
#define PER_PRODUCER 200000

static solco_cmdq_t s_queue;
static atomic_int s_go;

static int producer(void *arg)
{
    int id = (int)(intptr_t)arg;
    while (!atomic_load(&s_go))
        ;
    for (int i = 0; i < PER_PRODUCER;)
    {
        solco_cmd_t cmd = {.deck = (uint8_t)id, .value = i};
        if (solco_cmdq_push(&s_queue, &cmd))
            ++i;
        else
            thrd_yield();
    }
    return 0;
}

int main(void)
{
    solco_cmdq_init(&s_queue);
    solco_cmd_t cmd;
    assert(!solco_cmdq_pop(&s_queue, &cmd));
    for (int i = 0; i < SOLCO_CMDQ_SIZE; ++i)
    {
        cmd = (solco_cmd_t){.value = i};
        assert(solco_cmdq_push(&s_queue, &cmd));
    }
    cmd.value = -1;
    assert(!solco_cmdq_push(&s_queue, &cmd));
    assert(atomic_load(&s_queue.dropped) == 1);
    for (int i = 0; i < SOLCO_CMDQ_SIZE; ++i)
    {
        assert(solco_cmdq_pop(&s_queue, &cmd) && cmd.value == i);
    }
    assert(!solco_cmdq_pop(&s_queue, &cmd));

    solco_cmdq_init(&s_queue);
    thrd_t threads[PRODUCERS];
    for (int i = 0; i < PRODUCERS; ++i)
        assert(thrd_create(&threads[i], producer, (void *)(intptr_t)i) == thrd_success);
    atomic_store(&s_go, 1);
    int next[PRODUCERS] = {0};
    long received = 0;
    while (received < (long)PRODUCERS * PER_PRODUCER)
    {
        if (solco_cmdq_pop(&s_queue, &cmd))
        {
            assert(cmd.deck < PRODUCERS);
            assert(cmd.value == next[cmd.deck]); /* per-producer FIFO, no loss, no dup */
            ++next[cmd.deck];
            ++received;
        }
    }
    for (int i = 0; i < PRODUCERS; ++i)
        thrd_join(threads[i], NULL);
    assert(!solco_cmdq_pop(&s_queue, &cmd));
    printf("PASS cmdq: bounds, FIFO, %d producers x %d commands, no loss\n", PRODUCERS,
           PER_PRODUCER);
    return 0;
}
