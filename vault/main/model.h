#ifndef VAULT_MODEL_H
#define VAULT_MODEL_H

#include "format.h"
#include "cJSON.h"

#define VAULT_ENTRIES 512
typedef struct
{
    char id[33], name[65], email[129], username[129], password[257];
    char website[257], note[257], otp[105];
    unsigned otp_digits, otp_period, otp_hash;
} vault_entry_t;

typedef struct
{
    unsigned count;
    vault_entry_t entries[VAULT_ENTRIES];
} vault_model_t;

typedef enum
{
    VAULT_SKIP,
    VAULT_KEEP,
    VAULT_REPLACE,
    VAULT_DUPLICATE
} vault_resolution_t;

void vault_json_init(void);
void vault_secure_free(void *pointer);
void *vault_secure_alloc(size_t length);
bool vault_entry_valid(const vault_entry_t *entry);
cJSON *vault_entry_json(const vault_entry_t *entry, bool secrets);
bool vault_entry_parse(const cJSON *json, vault_entry_t *entry);
char *vault_model_json(const vault_model_t *model, const unsigned *indices, unsigned count);
bool vault_model_parse(const char *json, size_t length, vault_model_t *model);
int vault_model_conflict(const vault_model_t *model, const vault_entry_t *incoming);
bool vault_model_merge(vault_model_t *target, const vault_model_t *incoming,
                       const vault_resolution_t *choices);

#endif
