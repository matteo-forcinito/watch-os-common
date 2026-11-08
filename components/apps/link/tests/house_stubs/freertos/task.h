#ifndef HOUSE_TEST_TASK_H
#define HOUSE_TEST_TASK_H
#include <stdbool.h>
#include "FreeRTOS.h"
void vTaskDelay(unsigned ticks);
void vTaskDelete(void *task);
#endif
