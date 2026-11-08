#ifndef HOUSE_TEST_FREERTOS_H
#define HOUSE_TEST_FREERTOS_H
#include <stdbool.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdMS_TO_TICKS(ms) (ms)
static inline void portENTER_CRITICAL(portMUX_TYPE *mux) { (void)mux; }
static inline void portEXIT_CRITICAL(portMUX_TYPE *mux) { (void)mux; }
#endif
