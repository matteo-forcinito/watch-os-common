#ifndef CORE_SYS_H
#define CORE_SYS_H

/*
 * Small system helpers used everywhere.
 *
 * sys_task_create(): every background job (radio, web, installer, scans)
 * starts a task, and a task stack must live in internal RAM. If it cannot be
 * created the feature silently does nothing - the worst kind of bug on a
 * watch. This wrapper logs why (with the heap state) and returns false so
 * the caller can show an error.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SYS_CORE_ANY tskNO_AFFINITY

bool sys_task_create(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t priority,
                     BaseType_t core);

/*
 * Background worker: runs fn(arg) in the "sys" task (6 KB stack, low priority).
 *
 * esp_timer callbacks run in a small shared task that must never block: use
 * sys_post() from them for anything real (NVS writes, Wi-Fi on/off, alarm
 * checks, Bluetooth messages). Never blocks; returns false if the queue is full.
 */
typedef void (*sys_job_fn_t)(void *arg);
bool sys_post(sys_job_fn_t fn, void *arg);

/* Start the worker (first thing in app_main). */
void sys_worker_init(void);

/* "internal 123 KB free (largest 60 KB, min 80 KB), PSRAM 7.5 MB free" in the log. */
void sys_heap_log(const char *where);

/*
 * Why the chip started this time. If the previous run crashed, the log shows
 * the task and the address saved in the core dump. Call once at boot.
 */
void sys_check_last_reset(void);

/* "Spegnimento", "Errore: radio_dec @0x4200abcd"... for the Info page. */
const char *sys_last_reset_text(void);
bool sys_last_reset_was_crash(void);

/* Free internal RAM in bytes and the largest block. */
size_t sys_internal_free(void);
size_t sys_internal_largest(void);
size_t sys_internal_min_free(void);

#endif
