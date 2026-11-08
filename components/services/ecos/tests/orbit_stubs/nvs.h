#ifndef TEST_NVS_H
#define TEST_NVS_H
#include <stdint.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_u64(nvs_handle_t handle, const char *key, uint64_t *value);
esp_err_t nvs_set_u64(nvs_handle_t handle, const char *key, uint64_t value);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
#endif
