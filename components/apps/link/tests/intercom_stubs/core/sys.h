#ifndef INTERCOM_TEST_SYS_H
#define INTERCOM_TEST_SYS_H
#include <stdbool.h>
#include "freertos/task.h"
#define SYS_CORE_ANY 0
bool sys_task_create(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t priority, int core);
#endif
