#ifndef INTERCOM_TEST_FREERTOS_H
#define INTERCOM_TEST_FREERTOS_H
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t TickType_t;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#endif
