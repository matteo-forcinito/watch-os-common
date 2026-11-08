#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool orbit_state_get(const uint8_t *,size_t,uint8_t *,size_t,size_t *,uint64_t *,uint8_t[6]);
bool orbit_state_put(const uint8_t *,size_t,const uint8_t *,size_t);
bool orbit_state_delete(const uint8_t *,size_t);
