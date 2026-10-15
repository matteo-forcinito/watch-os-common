#ifndef CORE_RECOVERY_H
#define CORE_RECOVERY_H
#include <stdbool.h>
#include "esp_err.h"
/* Persistent headless boot; no dependency on panel, touch or network services. */
void recovery_init(void);
bool recovery_active(void);
bool recovery_supported(void);
esp_err_t recovery_set(bool enabled);
bool recovery_request(bool enabled);
#endif
