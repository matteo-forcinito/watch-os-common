/* One low-priority worker owns the live model, key and file commits.
 * Lock revokes access immediately, including in-flight typing operations. */
#include "vault.h"
#include "core/power.h"
#include "core/lv_port.h"
#include "extapp_sdk.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hardware/board.h"
#include "nvs.h"

/* Readers (UI, web, QA) wait briefly: the worker runs at idle priority and may hold
 * the mutex while preempted; priority inheritance lets it finish quickly. */
#define READ_WAIT pdMS_TO_TICKS(50)
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;
static vault_model_t *s_model;
static uint8_t s_key[32];
static vault_store_t s_store;
static vault_status_t s_status;
static atomic_bool s_open, s_busy, s_lock_requested;
static atomic_bool s_exit_requested;
static atomic_llong s_activity;
static int64_t s_retry_after;
static unsigned s_failures;
static vault_model_t *s_incoming;
static char s_preview[33];
static uint64_t s_preview_generation;
static int64_t s_preview_deadline;

const char *vault_field(const vault_entry_t *entry, unsigned field)
{
    return field == 1   ? entry->email
           : field == 2 ? entry->username
           : field == 3 ? entry->website
                        : entry->name;
}

unsigned vault_query(unsigned field, bool groups, bool grouped, const char *value, unsigned offset,
                     unsigned *indices, unsigned capacity, unsigned *total)
{
    if (total == NULL || indices == NULL || field > 3 || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, READ_WAIT) != pdTRUE)
        return 0;
    unsigned count = 0, matches = 0;
    if (vault_unlocked() && s_model != NULL)
    {
        for (unsigned i = 0; i < s_model->count; i++)
        {
            const char *text = vault_field(&s_model->entries[i], field);
            if (grouped && strcmp(text, value) != 0)
                continue;
            bool duplicate = false;
            if (groups)
                for (unsigned j = 0; j < i; j++)
                    if (strcmp(vault_field(&s_model->entries[j], field), text) == 0)
                    {
                        duplicate = true;
                        break;
                    }
            if (duplicate)
                continue;
            if (matches++ >= offset && count < capacity)
                indices[count++] = i;
        }
    }
    *total = matches;
    xSemaphoreGive(s_mutex);
    return count;
}

static void message(const char *text)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_status.message, sizeof(s_status.message), "%s", text);
    s_status.revision++;
    xSemaphoreGive(s_mutex);
}

bool vault_unlocked(void)
{
    return atomic_load(&s_open) && !atomic_load(&s_lock_requested);
}

void vault_activity(void)
{
    atomic_store(&s_activity, esp_timer_get_time());
}

void vault_lock(void)
{
    atomic_store(&s_open, false);
    atomic_store(&s_lock_requested, true);
}

void vault_exit(void)
{
    atomic_store(&s_exit_requested, true);
    vault_lock();
}

bool vault_crypto_should_cancel(void)
{
    return atomic_load(&s_lock_requested) || atomic_load(&s_exit_requested) ||
           power_screen() == POWER_SCREEN_OFF;
}

bool vault_status(vault_status_t *status)
{
    if (s_mutex == NULL || status == NULL || xSemaphoreTake(s_mutex, READ_WAIT) != pdTRUE)
        return false;
    *status = s_status;
    status->unlocked = vault_unlocked();
    status->count = status->unlocked && s_model != NULL ? s_model->count : 0;
    status->preview_count = status->unlocked && s_incoming != NULL ? s_incoming->count : 0;
    status->generation = status->unlocked ? s_store.header.generation : 0;
    int64_t elapsed = (esp_timer_get_time() - atomic_load(&s_activity)) / 1000000;
    status->remaining = status->unlocked && elapsed < 120 ? (unsigned)(120 - elapsed) : 0;
    status->busy = atomic_load(&s_busy);
    int64_t wait = s_retry_after - esp_timer_get_time();
    status->cooldown = wait > 0 ? (unsigned)((wait + 999999) / 1000000) : 0;
    xSemaphoreGive(s_mutex);
    return true;
}

vault_model_t *vault_preview_snapshot(char token[33], int conflicts[VAULT_ENTRIES])
{
    vault_model_t *copy = vault_secure_alloc(sizeof(*copy));
    if (copy == NULL)
        return NULL;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = vault_unlocked() && s_model != NULL && s_incoming != NULL &&
              esp_timer_get_time() < s_preview_deadline;
    if (ok)
    {
        *copy = *s_incoming;
        memcpy(token, s_preview, 33);
        for (unsigned i = 0; i < copy->count; i++)
            conflicts[i] = vault_model_conflict(s_model, &copy->entries[i]);
    }
    xSemaphoreGive(s_mutex);
    if (!ok)
    {
        vault_secure_free(copy);
        return NULL;
    }
    return copy;
}

bool vault_entry(unsigned index, vault_entry_t *entry)
{
    if (entry == NULL || s_mutex == NULL || xSemaphoreTake(s_mutex, READ_WAIT) != pdTRUE)
        return false;
    bool ok = vault_unlocked() && s_model != NULL && index < s_model->count;
    if (ok)
        *entry = s_model->entries[index];
    else
        vault_wipe(entry, sizeof(*entry));
    xSemaphoreGive(s_mutex);
    return ok;
}

bool vault_find_entry(const char *id, vault_entry_t *entry, unsigned *index)
{
    if (s_mutex == NULL || id == NULL || entry == NULL || index == NULL ||
        xSemaphoreTake(s_mutex, READ_WAIT) != pdTRUE)
        return false;
    bool ok = false;
    if (vault_unlocked() && s_model != NULL)
        for (unsigned i = 0; i < s_model->count; i++)
            if (strcmp(s_model->entries[i].id, id) == 0)
            {
                *entry = s_model->entries[i];
                *index = i;
                ok = true;
                break;
            }
    if (!ok)
        vault_wipe(entry, sizeof(*entry));
    xSemaphoreGive(s_mutex);
    return ok;
}

bool vault_preview_entry(unsigned index, vault_entry_t *entry, int *conflict, char token[33])
{
    if (s_mutex == NULL || entry == NULL || conflict == NULL || token == NULL ||
        xSemaphoreTake(s_mutex, READ_WAIT) != pdTRUE)
        return false;
    bool ok = vault_unlocked() && s_model != NULL && s_incoming != NULL &&
              index < s_incoming->count && esp_timer_get_time() < s_preview_deadline;
    if (ok)
    {
        *entry = s_incoming->entries[index];
        *conflict = vault_model_conflict(s_model, entry);
        memcpy(token, s_preview, 33);
    }
    else
        vault_wipe(entry, sizeof(*entry));
    xSemaphoreGive(s_mutex);
    return ok;
}

static void progress(unsigned percent)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_status.progress != percent)
    {
        s_status.progress = percent;
        s_status.revision++;
    }
    xSemaphoreGive(s_mutex);
}

vault_model_t *vault_snapshot(void)
{
    vault_model_t *copy = vault_secure_alloc(sizeof(*copy));
    if (copy == NULL)
        return NULL;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = vault_unlocked() && s_model != NULL;
    if (ok)
        *copy = *s_model;
    xSemaphoreGive(s_mutex);
    if (!ok)
    {
        vault_secure_free(copy);
        return NULL;
    }
    return copy;
}

uint8_t *vault_export(const unsigned *indices, unsigned count, size_t *size)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t *file = vault_unlocked() && s_model != NULL
                        ? vault_store_export(&s_store, s_key, s_model, indices, count, size)
                        : NULL;
    xSemaphoreGive(s_mutex);
    return file;
}

uint8_t *vault_export_selection(const vault_selection_t *selection, size_t *size)
{
    if (selection == NULL || selection->id_count > VAULT_ENTRIES || selection->field > 3 ||
        !vault_text_valid(selection->value, sizeof(selection->value), true))
        return NULL;
    unsigned indices[VAULT_ENTRIES], count = 0;
    uint8_t *file = NULL;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (vault_unlocked() && s_model != NULL)
    {
        for (unsigned i = 0; i < s_model->count; i++)
        {
            if (selection->grouped &&
                strcmp(vault_field(&s_model->entries[i], selection->field), selection->value) != 0)
                continue;
            bool selected = selection->id_count == 0;
            for (unsigned j = 0; j < selection->id_count; j++)
                if (strcmp(selection->ids[j], s_model->entries[i].id) == 0)
                    selected = true;
            if (selected)
                indices[count++] = i;
        }
        if (count > 0 && (selection->id_count == 0 || count == selection->id_count))
            file = vault_store_export(&s_store, s_key, s_model, indices, count, size);
    }
    xSemaphoreGive(s_mutex);
    return file;
}

void vault_job_free(vault_job_t *job)
{
    if (job == NULL)
        return;
    vault_secure_free(job->incoming);
    vault_secure_free(job->choices);
    vault_secure_free(job->selection);
    vault_secure_free(job->payload);
    vault_secure_free(job);
}

esp_err_t vault_submit(vault_job_t *job)
{
    if (job == NULL || s_queue == NULL)
        return ESP_ERR_INVALID_STATE;
    if (job->operation != VAULT_UNLOCK && job->operation != VAULT_CREATE &&
        job->operation != VAULT_EXIT && !vault_unlocked())
        return ESP_ERR_INVALID_STATE;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true))
        return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_queue, &job, 0) != pdTRUE)
    {
        atomic_store(&s_busy, false);
        return ESP_ERR_NO_MEM;
    }
    vault_activity();
    return ESP_OK;
}

static void cleanup_locked(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    vault_secure_free(s_model);
    s_model = NULL;
    vault_wipe(s_key, sizeof(s_key));
    vault_secure_free(s_incoming);
    s_incoming = NULL;
    vault_wipe(s_preview, sizeof(s_preview));
    s_status.revision++;
    snprintf(s_status.message, sizeof(s_status.message), "Archivio bloccato");
    xSemaphoreGive(s_mutex);
    lv_port_lock();
    vault_ui_scrub();
    lv_port_unlock();
    vault_hid_lock(true);
    vault_web_stop();
    atomic_store(&s_lock_requested, false);
}

static esp_err_t unlock(vault_job_t *job)
{
    if (s_status.storage == false || vault_unlocked() || esp_timer_get_time() < s_retry_after)
        return ESP_ERR_INVALID_STATE;
    vault_model_t *model = vault_secure_alloc(sizeof(*model));
    if (model == NULL)
        return ESP_ERR_NO_MEM;
    message("Derivazione chiave: attendi...");
    esp_err_t result = job->operation == VAULT_CREATE
                           ? vault_store_create(&s_store, job->password, s_key, model, progress)
                           : vault_store_unlock(&s_store, job->password, s_key, model, progress);
    vault_wipe(job->password, sizeof(job->password));
    if (result != ESP_OK || atomic_load(&s_lock_requested) || power_screen() == POWER_SCREEN_OFF)
    {
        vault_secure_free(model);
        vault_wipe(s_key, sizeof(s_key));
        if (result != ESP_OK)
        {
            s_failures++;
            unsigned wait = 1U << (s_failures > 8 ? 8 : s_failures);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_retry_after = esp_timer_get_time() + (int64_t)wait * 1000000;
            xSemaphoreGive(s_mutex);
        }
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_model = model;
    s_status.exists = s_store.exists;
    s_status.revision++;
    atomic_store(&s_open, true);
    xSemaphoreGive(s_mutex);
    s_failures = 0;
    vault_activity();
    vault_hid_lock(false);
    return ESP_OK;
}

static esp_err_t change(vault_job_t *job)
{
    if (job->operation != VAULT_IMPORT && job->expected_generation != s_store.header.generation)
        return ESP_ERR_INVALID_STATE;
    vault_model_t *candidate = vault_snapshot();
    if (candidate == NULL)
        return ESP_ERR_INVALID_STATE;
    bool valid = true;
    if (job->operation == VAULT_EDIT)
    {
        valid = vault_entry_valid(&job->entry) && job->index <= candidate->count;
        if (valid && job->index < candidate->count &&
            strcmp(candidate->entries[job->index].id, job->entry.id) != 0)
            valid = false;
        for (unsigned i = 0; valid && i < candidate->count; i++)
            if (i != job->index && strcmp(candidate->entries[i].id, job->entry.id) == 0)
                valid = false;
        if (valid)
        {
            if (job->index == candidate->count)
            {
                if (candidate->count == VAULT_ENTRIES)
                    valid = false;
                else
                    candidate->count++;
            }
            if (valid)
                candidate->entries[job->index] = job->entry;
        }
    }
    else if (job->operation == VAULT_DELETE)
    {
        valid = job->index < candidate->count &&
                strcmp(candidate->entries[job->index].id, job->entry.id) == 0;
        if (valid)
        {
            memmove(&candidate->entries[job->index], &candidate->entries[job->index + 1],
                    (candidate->count - job->index - 1) * sizeof(vault_entry_t));
            vault_wipe(&candidate->entries[--candidate->count], sizeof(vault_entry_t));
        }
    }
    else
    {
        valid = job->enabled && s_incoming != NULL && vault_equal(job->preview, s_preview, 33) &&
                s_preview_generation == s_store.header.generation &&
                esp_timer_get_time() < s_preview_deadline;
        if (valid)
            valid = vault_model_merge(candidate, s_incoming, job->choices);
    }
    vault_store_t next = s_store;
    esp_err_t result = valid && vault_unlocked() ? vault_store_commit(&next, s_key, candidate)
                                                 : ESP_ERR_INVALID_ARG;
    if (result == ESP_OK)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_store = next;
        if (vault_unlocked())
        {
            vault_model_t *old = s_model;
            s_model = candidate;
            candidate = old;
        }
        s_status.revision++;
        if (job->operation == VAULT_IMPORT)
        {
            vault_secure_free(s_incoming);
            s_incoming = NULL;
            vault_wipe(s_preview, 33);
        }
        xSemaphoreGive(s_mutex);
    }
    vault_secure_free(candidate);
    return result;
}

static esp_err_t preview(vault_job_t *job)
{
    if (job->payload == NULL || job->size > VAULT_MAX_PAYLOAD + VAULT_HEADER_SIZE + VAULT_TAG_SIZE)
        return ESP_ERR_INVALID_ARG;
    vault_header_t header;
    if (!vault_header_decode(job->payload, job->size, &header))
        return ESP_ERR_INVALID_ARG;
    uint8_t key[32] = {0};
    vault_model_t *incoming = vault_secure_alloc(sizeof(*incoming));
    uint8_t *plain = vault_secure_alloc(header.payload_size + 1);
    esp_err_t result =
        incoming != NULL && plain != NULL
            ? vault_derive(job->password, header.salt, header.iterations, key, progress)
            : ESP_ERR_NO_MEM;
    vault_wipe(job->password, sizeof(job->password));
    if (result == ESP_OK)
        result = vault_decrypt(key, job->payload, job->size, plain, header.payload_size, &header);
    if (result == ESP_OK && !vault_model_parse((const char *)plain, header.payload_size, incoming))
        result = ESP_ERR_INVALID_ARG;
    vault_wipe(key, 32);
    vault_secure_free(plain);
    uint8_t token[16];
    if (result == ESP_OK)
        result = vault_random(token, 16);
    if (result == ESP_OK && vault_unlocked())
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        vault_secure_free(s_incoming);
        s_incoming = incoming;
        incoming = NULL;
        static const char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < 16; i++)
        {
            s_preview[2 * i] = hex[token[i] >> 4];
            s_preview[2 * i + 1] = hex[token[i] & 15];
        }
        s_preview[32] = 0;
        s_preview_generation = s_store.header.generation;
        s_preview_deadline = esp_timer_get_time() + 300000000;
        s_status.revision++;
        xSemaphoreGive(s_mutex);
    }
    else if (result == ESP_OK)
        result = ESP_ERR_INVALID_STATE;
    vault_wipe(token, sizeof(token));
    vault_secure_free(incoming);
    return result;
}

static esp_err_t export_file(vault_job_t *job)
{
    size_t size = 0;
    uint8_t *file = vault_export_selection(job->selection, &size);
    if (file == NULL)
        return ESP_ERR_INVALID_ARG;
    char directory[128], name[160];
    snprintf(directory, sizeof(directory), "%s/exports", s_store.directory);
    mkdir(directory, 0700);
    snprintf(name, sizeof(name), "%s/backup-%llu-%lld.wvault", directory,
             (unsigned long long)s_store.header.generation, (long long)esp_timer_get_time());
    FILE *output = fopen(name, "wb");
    bool ok = output != NULL;
    if (ok)
    {
        ok = fwrite(file, 1, size, output) == size && fflush(output) == 0 &&
             fsync(fileno(output)) == 0;
        if (fclose(output) != 0)
            ok = false;
    }
    vault_secure_free(file);
    if (!ok)
    {
        unlink(name);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t import_file(vault_job_t *job)
{
    if (!vault_text_valid(job->filename, sizeof(job->filename), false) ||
        strcmp(job->filename, ".") == 0 || strcmp(job->filename, "..") == 0)
        return ESP_ERR_INVALID_ARG;
    for (const char *p = job->filename; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '-' || *p == '_'))
            return ESP_ERR_INVALID_ARG;
    char name[224];
    snprintf(name, sizeof(name), "%s/imports/%s", s_store.directory, job->filename);
    struct stat info;
    if (stat(name, &info) != 0 || info.st_size < VAULT_HEADER_SIZE + VAULT_TAG_SIZE ||
        info.st_size > VAULT_MAX_PAYLOAD + VAULT_HEADER_SIZE + VAULT_TAG_SIZE)
        return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(name, "rb");
    if (file == NULL)
        return ESP_FAIL;
    job->payload = vault_secure_alloc(info.st_size);
    job->size = info.st_size;
    bool ok = job->payload != NULL && fread(job->payload, 1, job->size, file) == job->size &&
              !ferror(file);
    fclose(file);
    return ok ? preview(job) : ESP_FAIL;
}

static esp_err_t interface(vault_job_t *job)
{
    if ((job->index != HID_LINK_BLE && job->index != HID_LINK_USB) || job->field > HID_HOST_US ||
        (job->index == HID_LINK_USB && !board_info()->has_usb_otg))
        return ESP_ERR_NOT_SUPPORTED;
    nvs_handle_t handle;
    esp_err_t result = nvs_open("vault_ui", NVS_READWRITE, &handle);
    if (result != ESP_OK)
        return result;
    result = nvs_set_u8(handle, "transport", job->index);
    if (result == ESP_OK)
        result = nvs_set_u8(handle, "layout", job->field);
    if (result == ESP_OK)
        result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

static void worker(void *unused)
{
    (void)unused;
    bool off_cleaned = false;
    for (;;)
    {
        if (power_screen() == POWER_SCREEN_OFF && !off_cleaned)
        {
            vault_lock();
            off_cleaned = true;
        }
        if (power_screen() != POWER_SCREEN_OFF)
            off_cleaned = false;
        if (vault_unlocked() && (esp_timer_get_time() - atomic_load(&s_activity) > 120000000 ||
                                 power_screen() == POWER_SCREEN_OFF))
            vault_lock();
        if (atomic_load(&s_lock_requested))
            cleanup_locked();
        if (atomic_load(&s_exit_requested))
        {
            cleanup_locked();
            extapp_return_to_launcher();
        }
        if (s_incoming != NULL && esp_timer_get_time() > s_preview_deadline)
        {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            vault_secure_free(s_incoming);
            s_incoming = NULL;
            vault_wipe(s_preview, 33);
            s_status.revision++;
            xSemaphoreGive(s_mutex);
        }
        vault_job_t *job = NULL;
        if (xQueueReceive(s_queue, &job, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.operation = job->operation;
        s_status.progress = 0;
        xSemaphoreGive(s_mutex);
        power_cpu_boost(true);
        power_keep_screen_on(true);
        esp_err_t result = ESP_ERR_INVALID_STATE;
        if (job->operation == VAULT_CREATE || job->operation == VAULT_UNLOCK)
            result = unlock(job);
        else if (job->operation == VAULT_EXIT)
        {
            vault_lock();
            cleanup_locked();
            vault_job_free(job);
            power_keep_screen_on(false);
            power_cpu_boost(false);
            extapp_return_to_launcher();
        }
        else if (vault_unlocked())
        {
            if (job->operation == VAULT_EDIT || job->operation == VAULT_DELETE ||
                job->operation == VAULT_IMPORT)
                result = change(job);
            else if (job->operation == VAULT_PREVIEW)
                result = preview(job);
            else if (job->operation == VAULT_EXPORT)
                result = export_file(job);
            else if (job->operation == VAULT_IMPORT_FILE)
                result = import_file(job);
            else if (job->operation == VAULT_INTERFACE)
            {
                result = interface(job);
                if (result == ESP_OK)
                {
                    vault_lock();
                    cleanup_locked();
                    vault_job_free(job);
                    power_keep_screen_on(false);
                    power_cpu_boost(false);
                    extapp_restart_self();
                }
            }
            else if (job->operation == VAULT_PREVIEW_CANCEL)
            {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                vault_secure_free(s_incoming);
                s_incoming = NULL;
                vault_wipe(s_preview, 33);
                s_status.revision++;
                xSemaphoreGive(s_mutex);
                result = ESP_OK;
            }
            else if (job->operation == VAULT_PREVIEW_EDIT)
            {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                if (s_incoming != NULL && vault_equal(job->preview, s_preview, 33) &&
                    job->index < s_incoming->count && vault_entry_valid(&job->entry) &&
                    strcmp(job->entry.id, s_incoming->entries[job->index].id) == 0)
                {
                    s_incoming->entries[job->index] = job->entry;
                    s_status.revision++;
                    result = ESP_OK;
                }
                xSemaphoreGive(s_mutex);
            }
            else if (job->operation == VAULT_WEB)
            {
                if (job->enabled)
                    result = vault_web_start();
                else
                {
                    vault_web_stop();
                    result = ESP_OK;
                }
            }
            else if (job->operation == VAULT_TYPE)
            {
                vault_entry_t entry;
                if (vault_entry(job->index, &entry) && strcmp(entry.id, job->entry.id) == 0)
                {
                    const char *text = job->field == 0   ? entry.email
                                       : job->field == 1 ? entry.username
                                       : job->field == 2 ? entry.password
                                                         : entry.website;
                    result = vault_hid_type(text);
                }
                vault_wipe(&entry, sizeof(entry));
            }
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.last_error = result;
        xSemaphoreGive(s_mutex);
        message(result == ESP_OK                ? "Operazione completata"
                : result == ESP_ERR_INVALID_CRC ? "Password errata o archivio danneggiato"
                : result == ESP_ERR_NOT_SUPPORTED
                    ? "Campo non scrivibile con questo layout"
                    : "Operazione non completata; dati originali conservati");
        vault_job_free(job);
        power_keep_screen_on(false);
        power_cpu_boost(false);
        atomic_store(&s_busy, false);
    }
}

esp_err_t vault_service_init(void)
{
    vault_json_init();
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(1, sizeof(vault_job_t *));
    if (s_mutex == NULL || s_queue == NULL)
        return ESP_ERR_NO_MEM;
    esp_err_t storage = vault_store_init(&s_store);
    s_status.storage = storage == ESP_OK;
    s_status.exists = s_store.exists;
    if (storage == ESP_OK)
    {
        char path[128];
        snprintf(path, sizeof(path), "%s/imports", s_store.directory);
        mkdir(path, 0700);
    }
    snprintf(s_status.message, sizeof(s_status.message), "%s",
             s_status.storage ? "Archivio bloccato" : "Inserisci una microSD e riapri Vault");
    vault_activity();
    /* Any core: the key derivation (crypto.c) raises itself to priority 2 while it runs. */
    if (xTaskCreatePinnedToCore(worker, "vault", 12288, NULL, tskIDLE_PRIORITY, NULL, tskNO_AFFINITY) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
