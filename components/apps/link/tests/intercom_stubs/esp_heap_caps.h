#ifndef INTERCOM_TEST_HEAP_H
#define INTERCOM_TEST_HEAP_H
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void *heap_caps_calloc(size_t n, size_t size, int caps);
void heap_caps_free(void *ptr);
#endif
