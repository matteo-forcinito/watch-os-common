/* Two encrypted snapshots. A failed save never replaces the live RAM model. */
#include "store.h"
#include "services/storage.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef VAULT_QA_DIRECTORY
#define VAULT_QA_DIRECTORY "vault-qa"
#endif

static void path(const vault_store_t *store, unsigned slot, char output[128])
{
    snprintf(output, 128, "%s/vault%u.wvault", store->directory, slot);
}

static uint8_t *read_file(const char *name, size_t *size)
{
    struct stat info;
    if (stat(name, &info) != 0 || info.st_size < VAULT_HEADER_SIZE + VAULT_TAG_SIZE ||
        info.st_size > VAULT_HEADER_SIZE + VAULT_TAG_SIZE + VAULT_MAX_PAYLOAD)
        return NULL;
    FILE *file = fopen(name, "rb");
    if (file == NULL)
        return NULL;
    uint8_t *bytes = vault_secure_alloc((size_t)info.st_size);
    bool ok = bytes != NULL &&
              fread(bytes, 1, (size_t)info.st_size, file) == (size_t)info.st_size && !ferror(file);
    fclose(file);
    if (!ok)
    {
        vault_secure_free(bytes);
        return NULL;
    }
    *size = (size_t)info.st_size;
    return bytes;
}

esp_err_t vault_store_init(vault_store_t *store)
{
    if (store == NULL)
        return ESP_ERR_INVALID_ARG;
    memset(store, 0, sizeof(*store));
    const char *root = storage_dir();
    if (root == NULL)
        return ESP_ERR_NOT_FOUND;
#if VAULT_QA
    /* QA builds only touch a synthetic fixture folder, never the real vault. */
    snprintf(store->directory, sizeof(store->directory), "%s/%s", root, VAULT_QA_DIRECTORY);
#else
    snprintf(store->directory, sizeof(store->directory), "%s/vault", root);
#endif
    if (mkdir(store->directory, 0700) != 0 && errno != EEXIST)
        return ESP_FAIL;
    for (unsigned i = 0; i < 2; i++)
    {
        char name[128];
        struct stat info;
        path(store, i, name);
        if (stat(name, &info) == 0)
            store->exists = true;
    }
    return ESP_OK;
}

uint8_t *vault_store_export(const vault_store_t *store, const uint8_t key[32],
                            const vault_model_t *model, const unsigned *indices, unsigned count,
                            size_t *size)
{
    char *json = vault_model_json(model, indices, count);
    if (json == NULL)
        return NULL;
    vault_header_t header = store->header;
    header.payload_size = (uint32_t)strlen(json);
    if (vault_random(header.nonce, sizeof(header.nonce)) != ESP_OK)
    {
        vault_secure_free(json);
        return NULL;
    }
    size_t length = VAULT_HEADER_SIZE + VAULT_TAG_SIZE + (size_t)header.payload_size;
    uint8_t *file = vault_secure_alloc(length);
    if (file != NULL && vault_encrypt(key, &header, (const uint8_t *)json, file, length) != ESP_OK)
    {
        vault_secure_free(file);
        file = NULL;
    }
    vault_secure_free(json);
    if (file != NULL)
        *size = length;
    return file;
}

esp_err_t vault_store_commit(vault_store_t *store, const uint8_t key[32],
                             const vault_model_t *model)
{
    if (store->header.generation == UINT64_MAX)
        return ESP_ERR_INVALID_STATE;
    vault_store_t next = *store;
    next.header.generation++;
    size_t size = 0;
    uint8_t *data = vault_store_export(&next, key, model, NULL, model->count, &size);
    if (data == NULL)
        return ESP_ERR_NO_MEM;
    char pending[128], destination[128];
    snprintf(pending, sizeof(pending), "%s/pending.wvault", store->directory);
    path(store, (unsigned)(next.header.generation & 1U), destination);
    FILE *file = fopen(pending, "wb");
    bool ok = file != NULL;
    if (ok)
    {
        ok = fwrite(data, 1, size, file) == size && fflush(file) == 0 && fsync(fileno(file)) == 0;
        if (fclose(file) != 0)
            ok = false;
    }
    /* Re-read and compare the entire authenticated envelope before publication. */
    if (ok)
    {
        size_t read_size = 0;
        uint8_t *check = read_file(pending, &read_size);
        ok = check != NULL && read_size == size && vault_equal(data, check, size);
        vault_secure_free(check);
    }
    vault_secure_free(data);
    if (ok)
    {
        if (unlink(destination) != 0 && errno != ENOENT)
            ok = false;
        if (ok)
            ok = rename(pending, destination) == 0;
    }
    if (!ok)
    {
        unlink(pending);
        return ESP_FAIL;
    }
    store->header = next.header;
    store->exists = true;
    return ESP_OK;
}

esp_err_t vault_store_create(vault_store_t *store, const char *password, uint8_t key[32],
                             vault_model_t *model, void (*progress)(unsigned))
{
    if (store->exists || !vault_text_valid(password, 129, false) || strlen(password) < 12)
        return ESP_ERR_INVALID_STATE;
    memset(&store->header, 0, sizeof(store->header));
    store->header.iterations = VAULT_KDF_ITERATIONS;
    if (vault_random(store->header.salt, 16) != ESP_OK)
        return ESP_FAIL;
    esp_err_t result =
        vault_derive(password, store->header.salt, store->header.iterations, key, progress);
    if (result != ESP_OK)
        return result;
    memset(model, 0, sizeof(*model));
    result = vault_store_commit(store, key, model);
    if (result != ESP_OK)
        vault_wipe(key, 32);
    return result;
}

esp_err_t vault_store_unlock(vault_store_t *store, const char *password, uint8_t key[32],
                             vault_model_t *model, void (*progress)(unsigned))
{
    uint8_t *files[2] = {0};
    size_t sizes[2] = {0};
    vault_header_t headers[2];
    bool valid[2] = {false, false};
    for (unsigned i = 0; i < 2; i++)
    {
        char name[128];
        path(store, i, name);
        files[i] = read_file(name, &sizes[i]);
        valid[i] = files[i] != NULL && vault_header_decode(files[i], sizes[i], &headers[i]);
    }
    unsigned first =
        valid[1] && (!valid[0] || headers[1].generation > headers[0].generation) ? 1 : 0;
    esp_err_t result = ESP_ERR_INVALID_CRC;
    bool derived = false;
    uint8_t previous_salt[16] = {0};
    uint32_t previous_cost = 0;
    for (unsigned attempt = 0; attempt < 2; attempt++)
    {
        unsigned i = first ^ attempt;
        if (!valid[i])
            continue;
        if (!derived || previous_cost != headers[i].iterations ||
            !vault_equal(previous_salt, headers[i].salt, 16))
        {
            result = vault_derive(password, headers[i].salt, headers[i].iterations, key, progress);
            if (result != ESP_OK)
                break;
            memcpy(previous_salt, headers[i].salt, 16);
            previous_cost = headers[i].iterations;
            derived = true;
        }
        uint8_t *plain = vault_secure_alloc(headers[i].payload_size + 1);
        if (plain == NULL)
        {
            result = ESP_ERR_NO_MEM;
            break;
        }
        result =
            vault_decrypt(key, files[i], sizes[i], plain, headers[i].payload_size, &headers[i]);
        if (result == ESP_OK)
        {
            plain[headers[i].payload_size] = 0;
            if (!vault_model_parse((const char *)plain, headers[i].payload_size, model))
                result = ESP_ERR_INVALID_CRC;
        }
        vault_secure_free(plain);
        if (result == ESP_OK)
        {
            store->header = headers[i];
            break;
        }
    }
    for (unsigned i = 0; i < 2; i++)
        vault_secure_free(files[i]);
    if (result != ESP_OK)
    {
        vault_wipe(key, 32);
        vault_wipe(model, sizeof(*model));
    }
    return result;
}
