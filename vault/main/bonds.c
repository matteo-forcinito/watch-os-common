/* Only this image redirects NimBLE's persistence namespace. Remote and the
 * companion keep their original identities and pairing records. */
#include "nvs.h"
#include <string.h>

esp_err_t __real_nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle);
esp_err_t __wrap_nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return __real_nvs_open(name != NULL && strcmp(name, "nimble_bond") == 0 ? "vault_bonds" : name,
                           mode, handle);
}
