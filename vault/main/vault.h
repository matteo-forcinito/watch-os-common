#ifndef VAULT_APP_H
#define VAULT_APP_H
#include "store.h"
#include "lvgl.h"
#include "hid_link/hid_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct
{
    bool unlocked, busy, exists, storage;
    unsigned count, revision, cooldown, preview_count, remaining, progress, operation;
    uint64_t generation;
    esp_err_t last_error;
    char message[128];
} vault_status_t;

typedef enum
{
    VAULT_CREATE,
    VAULT_UNLOCK,
    VAULT_EDIT,
    VAULT_DELETE,
    VAULT_IMPORT,
    VAULT_TYPE,
    VAULT_WEB,
    VAULT_EXIT,
    VAULT_PREVIEW,
    VAULT_PREVIEW_EDIT,
    VAULT_PREVIEW_CANCEL,
    VAULT_EXPORT,
    VAULT_INTERFACE,
    VAULT_IMPORT_FILE
} vault_operation_t;

typedef struct
{
    unsigned field, id_count;
    bool grouped;
    char value[257];
    char ids[VAULT_ENTRIES][33];
} vault_selection_t;

typedef struct
{
    vault_operation_t operation;
    vault_entry_t entry;
    char password[129];
    unsigned index, field;
    bool enabled;
    vault_model_t *incoming;
    vault_resolution_t *choices;
    vault_selection_t *selection;
    uint8_t *payload;
    size_t size;
    char preview[33];
    uint64_t expected_generation;
    char filename[97];
} vault_job_t;

esp_err_t vault_service_init(void);
esp_err_t vault_submit(vault_job_t *job); /* Ownership transfers only on ESP_OK. */
void vault_job_free(vault_job_t *job);
void vault_lock(void); /* Immediate access revocation; cleanup in worker. */
void vault_exit(void);
bool vault_unlocked(void);
void vault_activity(void);
bool vault_status(vault_status_t *status);
bool vault_entry(unsigned index, vault_entry_t *entry);
bool vault_find_entry(const char *id, vault_entry_t *entry, unsigned *index);
bool vault_preview_entry(unsigned index, vault_entry_t *entry, int *conflict, char token[33]);
vault_model_t *vault_snapshot(void);
uint8_t *vault_export(const unsigned *indices, unsigned count, size_t *size);
uint8_t *vault_export_selection(const vault_selection_t *selection, size_t *size);
vault_model_t *vault_preview_snapshot(char token[33], int conflicts[VAULT_ENTRIES]);
const char *vault_field(const vault_entry_t *entry, unsigned field);
unsigned vault_query(unsigned field, bool groups, bool grouped, const char *value, unsigned offset,
                     unsigned *indices, unsigned capacity, unsigned *total);
void vault_ui_create(lv_obj_t *screen);
bool vault_ui_back(void);
void vault_alarm_prepare(void);
void vault_alarm_start(void);
esp_err_t vault_web_start(void);
void vault_web_stop(void);
void vault_web_info(char *url, size_t url_size, char *code, size_t code_size, char *fingerprint,
                    size_t fingerprint_size);
esp_err_t vault_hid_init(void);
void vault_hid_lock(bool locked);
void vault_hid_pair(void);
unsigned vault_hid_passkey(void);
bool vault_hid_authenticated(void);
esp_err_t vault_hid_type(const char *text);
void vault_hid_caps(bool enabled);
void vault_input_init(void);
void vault_input_cancel(void);
void vault_input_scrub(void);
void vault_ui_scrub(void);
void vault_input(const char *title, const char *initial, bool password,
                 void (*callback)(const char *, void *), void *argument);
#endif
