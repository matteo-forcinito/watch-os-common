#ifndef HOUSE_TEST_SYS_H
#define HOUSE_TEST_SYS_H
#include <stdbool.h>
#define SYS_CORE_ANY 0
bool sys_task_create(void (*fn)(void *), const char *name, int stack,
                     void *arg, int priority, int core);
#endif
