#ifndef INTERCOM_TEST_TASK_H
#define INTERCOM_TEST_TASK_H
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(void *task);
#endif
