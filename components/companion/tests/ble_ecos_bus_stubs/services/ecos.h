#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef bool (*ecos_ble_send_t)(const uint8_t *frame,size_t length,void *context);
bool ecos_family_ble_open(ecos_ble_send_t send,void *context,const uint8_t mac[6]);
void ecos_family_ble_close(void *context);
bool ecos_family_ble_receive(const uint8_t *frame,size_t length,void *context);
