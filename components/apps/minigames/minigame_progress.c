/* Continuing games store a small versioned logical snapshot, not a frame.
 * The sys worker serializes flash commits. Cache copies stay under a short
 * mutex; the mutex is never held during NVS access. No UI pointer is queued. */
#include "minigame_progress.h"
#include "../pet/pet_game_catalog.h"
#include "core/sys.h"
#include "services/storage.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define PROGRESS_SLOTS 8
#define PROGRESS_MAGIC 0x4D475031u
#define PACKED_MAGIC 0x4D475A31u
typedef struct
{
    uint32_t magic, checksum;
    uint16_t version, size;
    uint8_t data[MG_CHECKPOINT_BYTES];
} checkpoint_t;
typedef struct
{
    char id[16];
    checkpoint_t checkpoint;
    uint32_t generation, committed, queued, disk_sequence;
    bool failed;
} progress_slot_t;
typedef struct { int slot; uint32_t generation; checkpoint_t checkpoint; } save_job_t;
static const char *TAG = "game_progress";
static progress_slot_t *s_slots;
static SemaphoreHandle_t s_mutex;
static char s_directory[96];
static void save_worker(void *arg);

/* PackBits-style zero runs keep sparse maps and unused entities inexpensive.
 * The checksum still covers the complete logical snapshot, including zeros. */
static size_t pack(const checkpoint_t *p, uint8_t *out)
{
    memcpy(out, p, 12);
    uint32_t magic = PACKED_MAGIC;
    memcpy(out, &magic, sizeof(magic));
    size_t used = 12, i = 0;
    while (i < p->size)
    {
        size_t zeros = 0;
        while (i + zeros < p->size && !p->data[i + zeros] && zeros < 128) zeros++;
        if (zeros >= 3)
        {
            out[used++] = (uint8_t)(128 + zeros - 1);
            i += zeros;
        }
        else
        {
            size_t begin = i++;
            while (i < p->size && i - begin < 128)
            {
                if (i + 2 < p->size && !p->data[i] && !p->data[i + 1] && !p->data[i + 2]) break;
                i++;
            }
            out[used++] = (uint8_t)(i - begin - 1);
            memcpy(out + used, p->data + begin, i - begin);
            used += i - begin;
        }
    }
    return used;
}
static bool unpack(checkpoint_t *p, const uint8_t *in, size_t bytes)
{
    if (bytes < 12) return false;
    memset(p, 0, sizeof(*p));
    memcpy(p, in, 12);
    if (p->size > MG_CHECKPOINT_BYTES) return false;
    if (p->magic == PROGRESS_MAGIC)
    {
        if (bytes != sizeof(*p)) return false;
        memcpy(p, in, bytes);
        return true;
    }
    if (p->magic != PACKED_MAGIC) return false;
    size_t pos = 12, used = 0;
    while (pos < bytes && used < p->size)
    {
        unsigned tag = in[pos++], count = (tag & 127) + 1;
        if (used + count > p->size) return false;
        if (!(tag & 128))
        {
            if (pos + count > bytes) return false;
            memcpy(p->data + used, in + pos, count);
            pos += count;
        }
        used += count;
    }
    p->magic = PROGRESS_MAGIC;
    return used == p->size && pos == bytes;
}

static uint32_t checksum(const checkpoint_t *p)
{
    uint32_t hash = 2166136261u ^ p->version ^ ((uint32_t)p->size << 16);
    for (unsigned i = 0; i < p->size; i++) hash = (hash ^ p->data[i]) * 16777619u;
    return hash;
}
static bool disk_load(progress_slot_t *slot, checkpoint_t *out)
{
    if (!s_directory[0]) return false;
    uint8_t *encoded = heap_caps_malloc(sizeof(checkpoint_t) + 32, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    checkpoint_t *candidate = heap_caps_malloc(sizeof(*candidate), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!encoded || !candidate) { free(encoded); free(candidate); return false; }
    bool found = false;
    for (int copy = 0; copy < 2; copy++)
    {
        char path[128];
        snprintf(path, sizeof(path), "%s/%s.%d", s_directory, slot->id, copy);
        FILE *file = fopen(path, "rb");
        if (!file) continue;
        uint32_t sequence = 0;
        bool header = fread(&sequence, sizeof(sequence), 1, file) == 1;
        size_t bytes = fread(encoded, 1, sizeof(checkpoint_t) + 32, file);
        bool complete = !ferror(file) && fgetc(file) == EOF;
        fclose(file);
        if (header && sequence && complete && unpack(candidate, encoded, bytes) &&
            candidate->checksum == checksum(candidate) &&
            (!found || (int32_t)(sequence - slot->disk_sequence) > 0))
        {
            *out = *candidate;
            slot->disk_sequence = sequence;
            found = true;
        }
    }
    free(encoded); free(candidate);
    return found;
}
static esp_err_t disk_save(progress_slot_t *slot, const uint8_t *encoded, size_t bytes)
{
    char path[128];
    uint32_t sequence = slot->disk_sequence + 1;
    if (!sequence) sequence = 1;
    if (mkdir(s_directory, 0775) != 0 && errno != EEXIST) return ESP_FAIL;
    snprintf(path, sizeof(path), "%s/%s.%u", s_directory, slot->id, (unsigned)(sequence & 1));
    FILE *file = fopen(path, "wb");
    if (!file) return ESP_FAIL;
    bool ok = fwrite(&sequence, sizeof(sequence), 1, file) == 1 &&
              fwrite(encoded, 1, bytes, file) == bytes && fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) return ESP_FAIL;
    slot->disk_sequence = sequence;
    return ESP_OK;
}
static bool supported(const mg_game_t *g)
{
    return g && g->id && g->continuous && g->save && g->load && g->checkpoint_version && g->checkpoint_bytes &&
           g->checkpoint_bytes <= MG_CHECKPOINT_BYTES && strlen(g->id) < 16;
}
void mg_progress_init(void)
{
    if (s_slots) return;
    const char *directory = storage_dir();
    s_directory[0] = 0;
    if (directory) snprintf(s_directory, sizeof(s_directory), "%s/game-saves", directory);
    s_mutex = xSemaphoreCreateMutex();
    s_slots = heap_caps_calloc(PROGRESS_SLOTS, sizeof(*s_slots), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_slots || !s_mutex)
    {
        free(s_slots);
        s_slots = NULL;
        if (s_mutex) vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        ESP_LOGE(TAG, "Checkpoint cache unavailable");
        return;
    }
    nvs_handle_t handle;
    esp_err_t status = nvs_open("pet_games", NVS_READONLY, &handle);
    int slot = 0;
    for (size_t i = 0; i < pet_game_count() && slot < PROGRESS_SLOTS; i++)
    {
        const mg_game_t *g = pet_game_at(i);
        if (!supported(g)) continue;
        progress_slot_t *s = &s_slots[slot++];
        snprintf(s->id, sizeof(s->id), "%s", g->id);
        checkpoint_t *p = &s->checkpoint;
        if (disk_load(s, p))
        {
            if (p->version == g->checkpoint_version && p->size == g->checkpoint_bytes)
                ESP_LOGI(TAG, "%s file checkpoint loaded (%u bytes)", s->id, p->size);
            else memset(p, 0, sizeof(*p));
            continue;
        }
        if (status != ESP_OK) continue;
        uint8_t *encoded = heap_caps_malloc(sizeof(*p) + 32, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t size = sizeof(*p) + 32;
        esp_err_t read = encoded ? nvs_get_blob(handle, s->id, encoded, &size) : ESP_ERR_NO_MEM;
        bool decoded = read == ESP_OK && unpack(p, encoded, size);
        free(encoded);
        if (!decoded || p->magic != PROGRESS_MAGIC ||
            p->version != g->checkpoint_version || p->size != g->checkpoint_bytes ||
            p->checksum != checksum(p))
            memset(p, 0, sizeof(*p));
        else
        {
            ESP_LOGI(TAG, "%s checkpoint loaded (%u bytes)", s->id, p->size);
            if (s_directory[0])
            {
                save_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (job)
                {
                    job->slot = slot - 1;
                    job->checkpoint = *p;
                    job->generation = s->generation = s->queued = 1;
                    if (!sys_post(save_worker, job)) { s->queued = 0; s->failed = true; free(job); }
                }
            }
        }
    }
    if (status == ESP_OK) nvs_close(handle);
}
static int find(const mg_game_t *g)
{
    if (!s_slots || !supported(g)) return -1;
    for (int i = 0; i < PROGRESS_SLOTS; i++) if (!strcmp(s_slots[i].id, g->id)) return i;
    return -1;
}
bool mg_progress_load(const mg_game_t *g, void *state)
{
    int index = find(g);
    if (index < 0) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const checkpoint_t *p = &s_slots[index].checkpoint;
    bool ok = p->magic == PROGRESS_MAGIC && p->version == g->checkpoint_version &&
              p->size == g->checkpoint_bytes && g->load(state, p->data, p->size);
    xSemaphoreGive(s_mutex);
    return ok;
}
static void save_worker(void *arg)
{
    save_job_t *job = arg;
    const char *id = s_slots[job->slot].id;
    nvs_handle_t handle;
    esp_err_t status = ESP_ERR_NO_MEM;
    uint8_t *encoded = heap_caps_malloc(sizeof(checkpoint_t) + 32, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (encoded)
    {
        size_t bytes = pack(&job->checkpoint, encoded);
        if (s_directory[0])
        {
            status = ESP_FAIL;
            if (storage_files_acquire())
            {
                status = disk_save(&s_slots[job->slot], encoded, bytes);
                storage_files_release();
            }
            /* Release a legacy NVS blob only after the file is durably written. */
            if (status == ESP_OK && nvs_open("pet_games", NVS_READWRITE, &handle) == ESP_OK)
            {
                if (nvs_erase_key(handle, id) == ESP_OK) nvs_commit(handle);
                nvs_close(handle);
            }
        }
        else
        {
            status = nvs_open("pet_games", NVS_READWRITE, &handle);
            if (status == ESP_OK)
            {
                status = nvs_set_blob(handle, id, encoded, bytes);
                if (status == ESP_OK) status = nvs_commit(handle);
                nvs_close(handle);
            }
        }
        free(encoded);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    progress_slot_t *slot = &s_slots[job->slot];
    if (slot->queued == job->generation) slot->queued = 0;
    if (job->generation == slot->generation) slot->failed = status != ESP_OK;
    if (status == ESP_OK) slot->committed = job->generation;
    xSemaphoreGive(s_mutex);
    if (status == ESP_OK)
    {
        ESP_LOGI(TAG, "%s checkpoint saved", id);
    }
    else ESP_LOGW(TAG, "%s save failed: %s", id, esp_err_to_name(status));
    free(job);
}
bool mg_progress_save(const mg_game_t *g, const void *state)
{
    int index = find(g);
    if (index < 0) return false;
    save_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) return false;
    job->slot = index;
    checkpoint_t *p = &job->checkpoint;
    p->magic = PROGRESS_MAGIC;
    p->version = g->checkpoint_version;
    p->size = g->checkpoint_bytes;
    if (!g->save(state, p->data, p->size)) { free(job); return false; }
    p->checksum = checksum(p);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    progress_slot_t *slot = &s_slots[index];
    bool same = !memcmp(&slot->checkpoint, p, sizeof(*p));
    if (same && (slot->committed == slot->generation || slot->queued == slot->generation))
    {
        xSemaphoreGive(s_mutex);
        free(job);
        return true;
    }
    slot->checkpoint = *p;
    job->generation = ++slot->generation;
    slot->queued = job->generation;
    xSemaphoreGive(s_mutex);
    if (sys_post(save_worker, job)) return true;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (slot->queued == job->generation) slot->queued = 0;
    slot->failed = true;
    xSemaphoreGive(s_mutex);
    free(job);
    return false;
}
bool mg_progress_failed(const mg_game_t *g)
{
    int index = find(g);
    if (index < 0) return g && g->continuous;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool failed = s_slots[index].failed;
    xSemaphoreGive(s_mutex);
    return failed;
}
