/* Strict schema, explicit import decisions and zeroed JSON allocations.
 * JSON hooks are private to this standalone firmware, never installed in OS. */
#include "model.h"
#include "crypto.h"
#include "esp_heap_caps.h"
#include "packet.h"
#include <stdlib.h>
#include <string.h>

typedef union
{
    size_t size;
    max_align_t alignment;
} secure_header_t;

void *vault_secure_alloc(size_t length)
{
    if (length > VAULT_MAX_PAYLOAD * 2U)
        return NULL;
    secure_header_t *header =
        heap_caps_malloc(sizeof(*header) + length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (header == NULL)
        return NULL;
    header->size = length;
    return header + 1;
}

void vault_secure_free(void *pointer)
{
    if (pointer == NULL)
        return;
    secure_header_t *header = (secure_header_t *)pointer - 1;
    size_t size = sizeof(*header) + header->size;
    vault_wipe(header, size);
    heap_caps_free(header);
}

void vault_json_init(void)
{
    cJSON_Hooks hooks = {.malloc_fn = vault_secure_alloc, .free_fn = vault_secure_free};
    cJSON_InitHooks(&hooks);
}

static bool identity(const char *id)
{
    if (strlen(id) != 32)
        return false;
    for (unsigned i = 0; i < 32; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            return false;
    return true;
}

bool vault_entry_valid(const vault_entry_t *entry)
{
    if (entry == NULL || !vault_text_valid(entry->id, sizeof(entry->id), false) ||
        !identity(entry->id) || !vault_text_valid(entry->name, sizeof(entry->name), false) ||
        !vault_text_valid(entry->email, sizeof(entry->email), true) ||
        !vault_text_valid(entry->username, sizeof(entry->username), true) ||
        !vault_text_valid(entry->password, sizeof(entry->password), true) ||
        !vault_text_valid(entry->website, sizeof(entry->website), true) ||
        !vault_text_valid(entry->note, sizeof(entry->note), true) ||
        !vault_text_valid(entry->otp, sizeof(entry->otp), true))
        return false;
    if ((entry->otp_digits != 6 && entry->otp_digits != 8) ||
        (entry->otp_period != 30 && entry->otp_period != 60) ||
        (entry->otp_hash != 1 && entry->otp_hash != 256 && entry->otp_hash != 512))
        return false;
    if (entry->otp[0])
    {
        uint8_t decoded[64];
        size_t length = 0;
        bool valid = vault_base32_decode(entry->otp, decoded, sizeof(decoded), &length);
        vault_wipe(decoded, sizeof(decoded));
        if (!valid || length < 10)
            return false;
    }
    return true;
}

cJSON *vault_entry_json(const vault_entry_t *entry, bool secrets)
{
    if (!vault_entry_valid(entry))
        return NULL;
    cJSON *object = cJSON_CreateObject();
    if (object == NULL)
        return NULL;
    bool ok = cJSON_AddStringToObject(object, "id", entry->id) &&
              cJSON_AddStringToObject(object, "name", entry->name) &&
              cJSON_AddStringToObject(object, "email", entry->email) &&
              cJSON_AddStringToObject(object, "username", entry->username) &&
              cJSON_AddStringToObject(object, "website", entry->website) &&
              cJSON_AddStringToObject(object, "note", entry->note) &&
              cJSON_AddNumberToObject(object, "otp_digits", entry->otp_digits) &&
              cJSON_AddNumberToObject(object, "otp_period", entry->otp_period) &&
              cJSON_AddNumberToObject(object, "otp_hash", entry->otp_hash);
    if (secrets)
        ok = ok && cJSON_AddStringToObject(object, "password", entry->password) &&
             cJSON_AddStringToObject(object, "otp", entry->otp);
    else
        ok = ok && cJSON_AddBoolToObject(object, "has_password", entry->password[0] != 0) &&
             cJSON_AddBoolToObject(object, "has_otp", entry->otp[0] != 0);
    if (!ok)
    {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static bool text(const cJSON *object, const char *name, char *output, size_t capacity)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || !vault_text_valid(item->valuestring, capacity, true))
        return false;
    memcpy(output, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

static bool number(const cJSON *object, const char *name, unsigned *output)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > 512 ||
        item->valuedouble != (unsigned)item->valuedouble)
        return false;
    *output = (unsigned)item->valuedouble;
    return true;
}

static bool keys(const cJSON *object, const char *const *names, unsigned count)
{
    if (!cJSON_IsObject(object) || count > 32)
        return false;
    uint32_t found = 0;
    for (const cJSON *item = object->child; item != NULL; item = item->next)
    {
        if (item->string == NULL)
            return false;
        unsigned i;
        for (i = 0; i < count; i++)
            if (strcmp(item->string, names[i]) == 0)
                break;
        if (i == count || (found & (1U << i)))
            return false;
        found |= 1U << i;
    }
    return found == ((1U << count) - 1);
}

bool vault_entry_parse(const cJSON *json, vault_entry_t *entry)
{
    static const char *const names[] = {"id",         "name",       "email",   "username",
                                        "password",   "website",    "note",    "otp",
                                        "otp_digits", "otp_period", "otp_hash"};
    if (entry == NULL)
        return false;
    memset(entry, 0, sizeof(*entry));
    if (!keys(json, names, sizeof(names) / sizeof(names[0])) ||
        !text(json, "id", entry->id, sizeof(entry->id)) ||
        !text(json, "name", entry->name, sizeof(entry->name)) ||
        !text(json, "email", entry->email, sizeof(entry->email)) ||
        !text(json, "username", entry->username, sizeof(entry->username)) ||
        !text(json, "password", entry->password, sizeof(entry->password)) ||
        !text(json, "website", entry->website, sizeof(entry->website)) ||
        !text(json, "note", entry->note, sizeof(entry->note)) ||
        !text(json, "otp", entry->otp, sizeof(entry->otp)) ||
        !number(json, "otp_digits", &entry->otp_digits) ||
        !number(json, "otp_period", &entry->otp_period) ||
        !number(json, "otp_hash", &entry->otp_hash) || !vault_entry_valid(entry))
    {
        vault_wipe(entry, sizeof(*entry));
        return false;
    }
    return true;
}

char *vault_model_json(const vault_model_t *model, const unsigned *indices, unsigned count)
{
    if (model == NULL || model->count > VAULT_ENTRIES || count > model->count)
        return NULL;
    if (indices == NULL)
        count = model->count;
    cJSON *root = cJSON_CreateObject();
    cJSON *array = root != NULL ? cJSON_AddArrayToObject(root, "entries") : NULL;
    bool ok = array != NULL && cJSON_AddNumberToObject(root, "schema", 1);
    for (unsigned i = 0; ok && i < count; i++)
    {
        unsigned index = indices != NULL ? indices[i] : i;
        if (index >= model->count)
        {
            ok = false;
            break;
        }
        for (unsigned j = 0; indices != NULL && j < i; j++)
            if (indices[j] == index)
                ok = false;
        cJSON *item = ok ? vault_entry_json(&model->entries[index], true) : NULL;
        if (item == NULL || !cJSON_AddItemToArray(array, item))
        {
            cJSON_Delete(item);
            ok = false;
        }
    }
    char *output = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (output != NULL && strlen(output) > VAULT_MAX_PAYLOAD)
    {
        vault_secure_free(output);
        output = NULL;
    }
    return output;
}

bool vault_model_parse(const char *json, size_t length, vault_model_t *model)
{
    if (model == NULL || length == 0 || length > VAULT_MAX_PAYLOAD || !sn_json_safe(json, length))
        return false;
    static const char *const names[] = {"schema", "entries"};
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema"),
          *entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    while (end != NULL && end < json + length &&
           (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t'))
        end++;
    bool ok = end == json + length && keys(root, names, 2) && cJSON_IsNumber(schema) &&
              schema->valuedouble == 1 && cJSON_IsArray(entries);
    int count = ok ? cJSON_GetArraySize(entries) : -1;
    ok = count >= 0 && count <= VAULT_ENTRIES;
    memset(model, 0, sizeof(*model));
    for (int i = 0; ok && i < count; i++)
    {
        ok = vault_entry_parse(cJSON_GetArrayItem(entries, i), &model->entries[i]);
        for (int j = 0; ok && j < i; j++)
            if (strcmp(model->entries[i].id, model->entries[j].id) == 0)
                ok = false;
    }
    cJSON_Delete(root);
    if (!ok)
    {
        vault_wipe(model, sizeof(*model));
        return false;
    }
    model->count = count;
    return true;
}

int vault_model_conflict(const vault_model_t *model, const vault_entry_t *incoming)
{
    if (model == NULL || incoming == NULL)
        return -1;
    for (unsigned i = 0; i < model->count; i++)
        if (strcmp(model->entries[i].id, incoming->id) == 0)
            return (int)i;
    for (unsigned i = 0; i < model->count; i++)
    {
        const vault_entry_t *old = &model->entries[i];
        if ((incoming->email[0] || incoming->username[0] || incoming->website[0]) &&
            strcmp(old->email, incoming->email) == 0 &&
            strcmp(old->username, incoming->username) == 0 &&
            strcmp(old->website, incoming->website) == 0)
            return (int)i;
    }
    return -1;
}

bool vault_model_merge(vault_model_t *target, const vault_model_t *incoming,
                       const vault_resolution_t *choices)
{
    if (target == NULL || incoming == NULL || choices == NULL || incoming->count > VAULT_ENTRIES)
        return false;
    /* The caller passes a disposable candidate; no live mutation before save. */
    for (unsigned i = 0; i < incoming->count; i++)
    {
        vault_resolution_t choice = choices[i];
        const vault_entry_t *entry = &incoming->entries[i];
        if (!vault_entry_valid(entry) || choice < VAULT_SKIP || choice > VAULT_DUPLICATE)
            return false;
        if (choice == VAULT_SKIP)
            continue;
        int index = vault_model_conflict(target, entry);
        if (index >= 0 && choice == VAULT_KEEP)
            continue;
        if (index >= 0 && choice == VAULT_REPLACE)
        {
            char id[33];
            memcpy(id, target->entries[index].id, sizeof(id));
            target->entries[index] = *entry;
            memcpy(target->entries[index].id, id, sizeof(id));
            continue;
        }
        if (target->count == VAULT_ENTRIES)
            return false;
        vault_entry_t copy = *entry;
        if (index >= 0)
        {
            uint8_t id[16];
            if (vault_random(id, sizeof(id)) != ESP_OK)
                return false;
            static const char hex[] = "0123456789abcdef";
            for (unsigned j = 0; j < 16; j++)
            {
                copy.id[2 * j] = hex[id[j] >> 4];
                copy.id[2 * j + 1] = hex[id[j] & 15];
            }
            copy.id[32] = 0;
            vault_wipe(id, sizeof(id));
        }
        target->entries[target->count++] = copy;
        vault_wipe(&copy, sizeof(copy));
    }
    return true;
}
