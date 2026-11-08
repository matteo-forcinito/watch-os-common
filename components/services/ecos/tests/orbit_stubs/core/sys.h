#ifndef TEST_CORE_SYS_H
#define TEST_CORE_SYS_H
#include <stdbool.h>
#define SYS_CORE_ANY 0
bool sys_post(void (*fn)(void *), void *arg);
bool sys_task_create(void (*fn)(void *), const char *name, int stack,
                     void *arg, int priority, int core);
#endif
