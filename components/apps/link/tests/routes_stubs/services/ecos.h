#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ECOS_ORBIT_MAX 144
bool ecos_family_stream_active(void);
void ecos_family_hold(bool);
int ecos_family_orbit(const char *,const uint8_t *,size_t,uint8_t *,size_t,int);
