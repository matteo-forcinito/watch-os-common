#ifndef VAULT_STORE_H
#define VAULT_STORE_H
#include "model.h"
#include "crypto.h"

typedef struct
{
    char directory[96];
    vault_header_t header;
    bool exists;
} vault_store_t;

esp_err_t vault_store_init(vault_store_t *store);
esp_err_t vault_store_unlock(vault_store_t *store, const char *password, uint8_t key[32],
                             vault_model_t *model, void (*progress)(unsigned));
esp_err_t vault_store_create(vault_store_t *store, const char *password, uint8_t key[32],
                             vault_model_t *model, void (*progress)(unsigned));
esp_err_t vault_store_commit(vault_store_t *store, const uint8_t key[32],
                             const vault_model_t *model);
uint8_t *vault_store_export(const vault_store_t *store, const uint8_t key[32],
                            const vault_model_t *model, const unsigned *indices, unsigned count,
                            size_t *size);
#endif
