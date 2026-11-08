#pragma once
#include <stddef.h>
#include <stdint.h>
#define ECOS_ORBIT_MAX 144
int ecos_family_orbit(const char *, const uint8_t *, size_t, uint8_t *, size_t, int);
