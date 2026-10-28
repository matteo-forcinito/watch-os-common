/* One low-priority importer; the storage producer always has higher priority. */
#include "prepare.h"
#include "mp3.h"
#include "wav.h"
#include "core/sys.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "psa/crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include <unistd.h>

typedef struct
{
    int deck;
    uint32_t generation;
    char source[256];
} job_t;
static QueueHandle_t s_jobs, s_results;
static uint32_t s_wanted[2];
static int s_percent[2];

void solco_prepare_cancel(int deck, uint32_t generation)
{
    __atomic_store_n(&s_wanted[deck], generation, __ATOMIC_RELEASE);
    __atomic_store_n(&s_percent[deck], 0, __ATOMIC_RELEASE);
}
int solco_prepare_percent(int deck)
{
    return __atomic_load_n(&s_percent[deck], __ATOMIC_ACQUIRE);
}
static bool current(const job_t *job)
{
    return __atomic_load_n(&s_wanted[job->deck], __ATOMIC_ACQUIRE) == job->generation;
}
static bool progress(int percent, void *context)
{
    job_t *job = context;
    if (!current(job))
    {
        return false;
    }
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    __atomic_store_n(&s_percent[job->deck], 10 + percent * 89 / 100, __ATOMIC_RELEASE);
    vTaskDelay(pdMS_TO_TICKS(1));
    return true;
}
static bool fingerprint(FILE *source, job_t *job, char hex[65])
{
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    unsigned char digest[32];
    size_t size = 0;
    unsigned char *buffer = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = buffer && psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
    size_t count;
    while (ok && (count = fread(buffer, 1, 8192, source)) > 0)
    {
        ok = current(job) && psa_hash_update(&operation, buffer, count) == PSA_SUCCESS;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ok = ok && !ferror(source) && psa_hash_finish(&operation, digest, sizeof(digest), &size) == PSA_SUCCESS &&
         size == 32;
    psa_hash_abort(&operation);
    free(buffer);
    if (!ok)
    {
        return false;
    }
    for (size_t i = 0; i < 32; ++i)
    {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    rewind(source);
    return true;
}
static void prepare(job_t *job, solco_prepared_t *result)
{
    FILE *input = fopen(job->source, "rb"), *output = NULL;
    char pending[144] = "", hash[65];
    bool published = false;
    snprintf(result->error, sizeof(result->error), "Preparazione MP3 non riuscita");
    if (!input || !fingerprint(input, job, hash))
    {
        goto done;
    }
    snprintf(result->path, sizeof(result->path), "/sdcard/music/.solco/p1-%s.wav", hash);
    FILE *existing = fopen(result->path, "rb");
    if (existing)
    {
        solco_wav_t format;
        bool valid = solco_wav_open(existing, &format);
        fclose(existing);
        if (valid)
        {
            published = true;
            goto done;
        }
    }
    uint64_t total = 0, available = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &available) != ESP_OK)
    {
        snprintf(result->error, sizeof(result->error), "Spazio microSD non leggibile");
        goto done;
    }
    const uint64_t reserve = 8 * 1024 * 1024;
    if (available <= reserve)
    {
        snprintf(result->error, sizeof(result->error), "Servono almeno 8 MB liberi");
        goto done;
    }
    mkdir("/sdcard/music/.solco", 0775);
    snprintf(pending, sizeof(pending), "%s.part", result->path);
    output = fopen(pending, "wb");
    if (!output)
    {
        goto done;
    }
    if (!solco_mp3_decode(input, output, available - reserve, progress, job, result->error,
                          sizeof(result->error)))
    {
        goto done;
    }
    bool ok = fsync(fileno(output)) == 0;
    ok = fclose(output) == 0 && ok;
    output = NULL;
    if (!ok || !current(job))
    {
        goto done;
    }
    /* Only our content-addressed cache entry can be replaced. */
    remove(result->path);
    published = rename(pending, result->path) == 0;
done:
    if (!published && !result->error[0])
    {
        snprintf(result->error, sizeof(result->error), "Errore salvataggio cache");
    }
    if (input)
    {
        fclose(input);
    }
    if (output)
    {
        fclose(output);
    }
    if (pending[0] && !published)
    {
        remove(pending);
    }
    if (published)
    {
        result->error[0] = '\0';
        __atomic_store_n(&s_percent[job->deck], 100, __ATOMIC_RELEASE);
    }
}
static void worker(void *context)
{
    (void)context;
    job_t job;
    while (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE)
    {
        if (!current(&job))
        {
            continue;
        }
        solco_prepared_t result = {.deck = job.deck, .generation = job.generation};
        prepare(&job, &result);
        if (current(&job))
        {
            xQueueSend(s_results, &result, portMAX_DELAY);
        }
    }
    vTaskDelete(NULL);
}
bool solco_prepare_request(int deck, uint32_t generation, const char *source)
{
    if (!s_jobs)
    {
        if (psa_crypto_init() != PSA_SUCCESS)
        {
            return false;
        }
        s_jobs = xQueueCreate(4, sizeof(job_t));
        s_results = xQueueCreate(4, sizeof(solco_prepared_t));
        /* With the kit radio up, internal RAM rarely has a free 32 KB block:
         * fall back to a PSRAM stack (the worker only touches SD and PSRAM). */
        bool started = s_jobs && s_results &&
                       (sys_task_create(worker, "solco_mp3", 32768, NULL, 2, 0) ||
                        xTaskCreatePinnedToCoreWithCaps(worker, "solco_mp3", 32768, NULL, 2, NULL, 0,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS);
        if (!started)
        {
            if (s_jobs)
            {
                vQueueDelete(s_jobs);
            }
            if (s_results)
            {
                vQueueDelete(s_results);
            }
            s_jobs = s_results = NULL;
            return false;
        }
    }
    job_t job = {.deck = deck, .generation = generation};
    snprintf(job.source, sizeof(job.source), "%s", source);
    return xQueueSend(s_jobs, &job, 0) == pdTRUE;
}
bool solco_prepare_poll(solco_prepared_t *result)
{
    return s_results && xQueueReceive(s_results, result, 0) == pdTRUE;
}
