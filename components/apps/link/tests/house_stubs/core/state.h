#ifndef HOUSE_TEST_STATE_H
#define HOUSE_TEST_STATE_H
#include <stdint.h>
typedef enum { STATE_BATTERY, STATE_USB } state_id_t;
int32_t state_get(state_id_t id);
#endif
