#ifndef ALARM_TEST_ESP_ERR_H
#define ALARM_TEST_ESP_ERR_H
typedef int esp_err_t;
enum
{
    ESP_OK = 0,
    ESP_FAIL = -1,
    ESP_ERR_INVALID_ARG = -2,
    ESP_ERR_NO_MEM = -3,
    ESP_ERR_NOT_FOUND = -4,
    ESP_ERR_NVS_INVALID_LENGTH = -5
};
#endif
