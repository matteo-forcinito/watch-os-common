#pragma once
#include "esp_err.h"
esp_err_t wifi_radio_acquire(void);
void wifi_radio_release(void);
bool wifi_radio_set_channel(uint8_t channel);
uint8_t wifi_radio_channel(void);
uint8_t wifi_home_channel(void);
