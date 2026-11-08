#pragma once
#include "ble_ecos.h"
/* Caller authenticates/encrypts the BLE connection before accepting requests. */
void ble_ecos_bus_init(ble_ecos_send_t send);
void ble_ecos_bus_request(const cJSON *request, unsigned epoch);
void ble_ecos_bus_disconnect(unsigned epoch);
