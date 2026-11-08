#ifndef HOUSE_TEST_ESP_MAC_H
#define HOUSE_TEST_ESP_MAC_H
#include "esp_err.h"
#define ESP_MAC_WIFI_STA 0
esp_err_t esp_read_mac(unsigned char *mac, int type);
#endif
