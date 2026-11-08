#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stdbool.h>
typedef unsigned int TickType_t;
typedef int portMUX_TYPE;
typedef struct { bool locked; } StaticSemaphore_t;
#define pdTRUE 1
#define portMAX_DELAY ((TickType_t)0xffffffffu)
#define portMUX_INITIALIZER_UNLOCKED 0
static inline void portENTER_CRITICAL(portMUX_TYPE *mux) { (void)mux; }
static inline void portEXIT_CRITICAL(portMUX_TYPE *mux) { (void)mux; }
#endif
