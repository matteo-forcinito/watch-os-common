#ifndef INTERCOM_TEST_ECOS_H
#define INTERCOM_TEST_ECOS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ECOS_ORBIT_MAX 144
typedef void (*ecos_stream_handler_t)(const char *, const uint8_t *, size_t);
void ecos_family_hold(bool on);
int ecos_family_orbit(const char *id, const uint8_t *request, size_t length, uint8_t *reply, size_t max, int timeout_ms);
bool ecos_family_stream_open(const char *id);
void ecos_family_stream_close(const char *id);
bool ecos_family_stream_send(const char *id, const uint8_t *data, size_t length);
void ecos_family_set_stream_handler(ecos_stream_handler_t handler);
#endif
