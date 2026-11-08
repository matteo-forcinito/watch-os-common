#ifndef HOUSE_TEST_SERVICES_ECOS_ORBIT_H
#define HOUSE_TEST_SERVICES_ECOS_ORBIT_H
#include <stddef.h>
#include <stdint.h>
#include "services/ecos.h"
#define ECOS_ORBIT_MAX 144
#define ECOS_ORBIT_REQUEST_MAX 1024
int ecos_family_orbit_large(const char *id, const uint8_t *request, size_t length,
                            uint8_t *reply, size_t max, int timeout_ms);
#endif
