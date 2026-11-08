#ifndef INTERCOM_TEST_QUEUE_H
#define INTERCOM_TEST_QUEUE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
typedef struct { uint8_t *storage; size_t item_size, capacity, head, count; bool deleted; } StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size, uint8_t *storage, StaticQueue_t *queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
void vQueueDelete(QueueHandle_t queue);
#endif
