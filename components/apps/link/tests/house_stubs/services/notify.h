#ifndef HOUSE_TEST_SERVICES_NOTIFY_H
#define HOUSE_TEST_SERVICES_NOTIFY_H
#include <time.h>
#include <stdint.h>
typedef struct {
    uint32_t id;
    time_t time;
    char app[24], title[48], body[200];
} notify_t;
void notify_add(const notify_t *notification);
#endif
