#ifndef ECOS_TEST_STRING_H
#define ECOS_TEST_STRING_H
#include <stddef.h>
void *memcpy(void *restrict to, const void *restrict from, size_t size);
int memcmp(const void *a, const void *b, size_t size);
void *memset(void *to, int value, size_t size);
char *strncpy(char *restrict to, const char *restrict from, size_t size);
#endif
