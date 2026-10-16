#include "services/voice_memo.h"

#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "hardware/audio.h"
#include "hardware/microphone.h"
#include "hardware/sdcard.h"
#include "services/sound.h"
#include "services/storage.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RATE 16000
#define CHUNK 512
#define RESERVE_MB 4
#define MAX_BYTES (RATE * 2 * VOICE_MEMO_LIMIT_SECONDS)

static const char *TAG = "voice_memo";
static portMUX_TYPE s_guard = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR voice_memo_snapshot_t s_state;
static bool s_busy = false;
static bool s_stop = false;
static int s_volume = -1;

typedef enum { RECORD, PLAY, SCAN, DELETE } operation_t;
typedef struct { operation_t operation; char name[VOICE_MEMO_NAME_SIZE]; } command_t;

static void publish(voice_memo_phase_t phase, uint32_t bytes, uint32_t total)
{
    portENTER_CRITICAL(&s_guard);
    s_state.phase = phase;
    s_state.elapsed_ms = bytes / (RATE * 2 / 1000);
    s_state.duration_ms = total / (RATE * 2 / 1000);
    portEXIT_CRITICAL(&s_guard);
}

void voice_memo_snapshot(voice_memo_snapshot_t *out)
{
    if (out != NULL)
    {
        portENTER_CRITICAL(&s_guard);
        *out = s_state;
        portEXIT_CRITICAL(&s_guard);
    }
}

void voice_memo_stop(void)
{
    __atomic_store_n(&s_stop, true, __ATOMIC_RELEASE);
}

int voice_memo_volume(void)
{
    int volume = __atomic_load_n(&s_volume, __ATOMIC_RELAXED);
    return volume < 0 ? settings_get()->volume : volume;
}

void voice_memo_set_volume(int percent)
{
    __atomic_store_n(&s_volume, percent < 0 ? 0 : percent > 100 ? 100 : percent, __ATOMIC_RELAXED);
}

static bool stopped(void)
{
    return __atomic_load_n(&s_stop, __ATOMIC_ACQUIRE) || sound_alarm_playing();
}

static void levels(const int16_t *buffer, size_t count)
{
    uint32_t peak = 0, clipped = 0;
    uint64_t squares = 0;
    for (size_t i = 0; i < count; i++)
    {
        int32_t value = buffer[i];
        uint32_t amplitude = value < 0 ? (uint32_t)-value : (uint32_t)value;
        if (amplitude > peak)
        {
            peak = amplitude;
        }
        squares += (int64_t)value * value;
        clipped += amplitude >= 32767;
    }
    int db = peak > 0 ? (int)(20.0f * log10f((float)peak / 32768.0f)) : -60;
    uint16_t rms = count > 0 ? (uint16_t)sqrtf((float)squares / count) : 0;
    portENTER_CRITICAL(&s_guard);
    s_state.peak = (uint16_t)peak;
    s_state.rms = rms;
    s_state.clipped += clipped;
    memmove(s_state.levels, s_state.levels + 1, sizeof(s_state.levels) - 1);
    s_state.levels[sizeof(s_state.levels) - 1] = db < -60 ? 0 : (uint8_t)(db + 60);
    portEXIT_CRITICAL(&s_guard);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void wav_header(uint8_t *header, uint32_t bytes)
{
    memset(header, 0, 44);
    memcpy(header, "RIFF", 4);
    put32(header + 4, bytes + 36);
    memcpy(header + 8, "WAVEfmt ", 8);
    put32(header + 16, 16);
    put16(header + 20, 1);
    put16(header + 22, 1);
    put32(header + 24, RATE);
    put32(header + 28, RATE * 2);
    put16(header + 32, 2);
    put16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    put32(header + 40, bytes);
}

static bool valid_header(const uint8_t *header)
{
    uint8_t expected[44];
    wav_header(expected, get32(header + 40));
    return memcmp(expected, header, sizeof(expected)) == 0;
}

static bool name_valid(const char *name, const char *suffix)
{
    size_t length = strlen(name), tail = strlen(suffix);
    if (length >= VOICE_MEMO_NAME_SIZE || length < 6 + tail || strncmp(name, "memo-", 5) != 0 ||
        strcmp(name + length - tail, suffix) != 0)
    {
        return false;
    }
    for (size_t i = 5; i < length - tail; i++)
    {
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f') || name[i] == '-'))
        {
            return false;
        }
    }
    return true;
}

static time_t memo_timestamp(const char *name)
{
    int year, month, day, hour, minute, second;
    if (sscanf(name, "memo-%4d%2d%2d-%2d%2d%2d-", &year, &month, &day, &hour, &minute, &second) == 6 &&
        year >= 2020 && year <= 2099 && month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
        hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59)
    {
        struct tm date = {.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
                          .tm_hour = hour, .tm_min = minute, .tm_sec = second, .tm_isdst = -1};
        return mktime(&date);
    }
    if (strlen(name) == 21 && name[13] == '-')
    {
        return (time_t)strtoul(name + 5, NULL, 16);
    }
    return 0;
}

void voice_memo_format_name(const char *name, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0)
    {
        return;
    }
    time_t timestamp = memo_timestamp(name);
    struct tm date;
    if (timestamp > 0 && localtime_r(&timestamp, &date) != NULL && date.tm_year >= 120)
    {
        char stamp[40];
        strftime(stamp, sizeof(stamp), "%d/%m/%Y  %H:%M:%S", &date);
        bool probe = strlen(name) >= 8 && strcmp(name + strlen(name) - 8, "-999.wav") == 0;
        snprintf(out, capacity, "%s%s", probe ? "Prova audio · " : "", stamp);
    }
    else
    {
        snprintf(out, capacity, "%s", name);
    }
}

/* Only our canonical PCM files are recovered. Unknown files are untouched. */
static void recover(const char *dir, const char *name)
{
    char part[128], final[128];
    snprintf(part, sizeof(part), "%s/%s", dir, name);
    snprintf(final, sizeof(final), "%s/%.*s", dir, (int)strlen(name) - 5, name);
    struct stat info;
    if (stat(final, &info) == 0 || stat(part, &info) != 0 || info.st_size < 46 || info.st_size > MAX_BYTES + 44)
    {
        return;
    }
    FILE *file = fopen(part, "r+b");
    if (file == NULL)
    {
        return;
    }
    uint8_t header[44];
    bool ok = fread(header, 1, 44, file) == 44 && valid_header(header);
    uint32_t bytes = (uint32_t)(info.st_size - 44) & ~1U;
    if (ok)
    {
        wav_header(header, bytes);
        ok = ftruncate(fileno(file), bytes + 44) == 0 && fseek(file, 0, SEEK_SET) == 0 &&
             fwrite(header, 1, 44, file) == 44 && fflush(file) == 0 && fsync(fileno(file)) == 0;
    }
    ok = fclose(file) == 0 && ok;
    if (ok && rename(part, final) == 0)
    {
        ESP_LOGI(TAG, "Recovered %lu PCM bytes", (unsigned long)bytes);
    }
}

static esp_err_t scan(const char *dir)
{
    DIR *folder = opendir(dir);
    if (folder == NULL)
    {
        return ESP_FAIL;
    }
    voice_memo_snapshot_t *list = heap_caps_calloc(1, sizeof(*list), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (list == NULL)
    {
        closedir(folder);
        return ESP_ERR_NO_MEM;
    }
    struct dirent *entry;
    int examined = 0;
    time_t timestamps[VOICE_MEMO_LIST_SIZE] = {0};
    while ((entry = readdir(folder)) != NULL)
    {
        if (++examined % 32 == 0)
        {
            vTaskDelay(1);
        }
        if (name_valid(entry->d_name, ".wav.part"))
        {
            recover(dir, entry->d_name);
        }
    }
    rewinddir(folder);
    examined = 0;
    while ((entry = readdir(folder)) != NULL)
    {
        if (++examined % 32 == 0)
        {
            vTaskDelay(1);
        }
        if (!name_valid(entry->d_name, ".wav"))
        {
            continue;
        }
        /* Keep the newest 20 names; older memos remain available on the card. */
        int pos = 0;
        time_t timestamp = memo_timestamp(entry->d_name);
        while (pos < list->count && (timestamps[pos] > timestamp ||
               (timestamps[pos] == timestamp && strcmp(list->names[pos], entry->d_name) > 0)))
        {
            pos++;
        }
        if (pos >= VOICE_MEMO_LIST_SIZE)
        {
            continue;
        }
        if (list->count < VOICE_MEMO_LIST_SIZE)
        {
            list->count++;
        }
        for (int i = list->count - 1; i > pos; i--)
        {
            memcpy(list->names[i], list->names[i - 1], VOICE_MEMO_NAME_SIZE);
            timestamps[i] = timestamps[i - 1];
        }
        timestamps[pos] = timestamp;
        snprintf(list->names[pos], VOICE_MEMO_NAME_SIZE, "%.*s", VOICE_MEMO_NAME_SIZE - 1, entry->d_name);
    }
    closedir(folder);
    portENTER_CRITICAL(&s_guard);
    s_state.count = list->count;
    s_state.selected = 0;
    memcpy(s_state.names, list->names, sizeof(s_state.names));
    portEXIT_CRITICAL(&s_guard);
    free(list);
    return ESP_OK;
}

static bool space_available(void)
{
    uint32_t total = 0, free_mb = 0;
    sdcard_space(&total, &free_mb);
    return sdcard_is_mounted() && free_mb >= RESERVE_MB;
}

static esp_err_t record(const char *dir, int16_t *buffer)
{
    if (!space_available() || stopped())
    {
        return stopped() ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = ESP_OK;
    char final[120], part[128];
    struct stat info;
    unsigned counter = 0;
    time_t now = time(NULL);
    struct tm date;
    char stamp[24];
    if (localtime_r(&now, &date) != NULL && date.tm_year >= 120 && date.tm_year <= 199)
    {
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &date);
    }
    else
    {
        snprintf(stamp, sizeof(stamp), "%08lx", (unsigned long)now);
    }
    do
    {
        snprintf(final, sizeof(final), "%s/memo-%s-%03u.wav", dir, stamp, counter++);
        snprintf(part, sizeof(part), "%s.part", final);
    } while ((stat(final, &info) == 0 || stat(part, &info) == 0) && counter < 1000);
    FILE *file = counter < 1000 ? fopen(part, "wb") : NULL;
    uint32_t bytes = 0;
    uint8_t header[44];
    wav_header(header, 0);
    bool ok = file != NULL && fwrite(header, 1, 44, file) == 44;
    if (!ok)
    {
        err = ESP_FAIL;
    }
    if (ok && !stopped())
    {
        for (int attempt = 0; attempt < 25 && !stopped(); attempt++)
        {
            err = microphone_start(RATE, 1);
            if (err != ESP_ERR_INVALID_STATE)
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    /* Drain the codec's analogue settling transient before saving PCM or
     * measuring levels. Reading continuously also prevents DMA overflow. */
    for (int warmup = 0; ok && err == ESP_OK && !stopped() && warmup < 16; warmup++)
    {
        size_t count = 0;
        err = microphone_read(buffer, CHUNK, &count);
        if (err == ESP_OK && count == 0)
        {
            err = ESP_ERR_TIMEOUT;
        }
    }
    sys_heap_log("memo-record");
    while (ok && err == ESP_OK && !stopped() && bytes < MAX_BYTES)
    {
        size_t count = 0;
        err = microphone_read(buffer, CHUNK, &count);
        if (err != ESP_OK || count == 0)
        {
            break;
        }
        levels(buffer, count);
        size_t written = fwrite(buffer, sizeof(int16_t), count, file);
        bytes += written * sizeof(int16_t);
        if (written != count || ((bytes / 1024) % 32 == 0 && !space_available()))
        {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        publish(VOICE_MEMO_RECORDING, bytes, MAX_BYTES);
    }
    /* Free the bus before filesystem finalisation, so the alarm can ring. */
    microphone_stop();
    publish(VOICE_MEMO_SAVING, bytes, MAX_BYTES);
    if (file != NULL)
    {
        wav_header(header, bytes);
        ok = ok && fseek(file, 0, SEEK_SET) == 0 && fwrite(header, 1, 44, file) == 44 &&
             fflush(file) == 0 && fsync(fileno(file)) == 0;
        ok = fclose(file) == 0 && ok;
        if (ok && bytes != 0)
        {
            ok = rename(part, final) == 0;
        }
        else if (ok)
        {
            remove(part);
        }
    }
    ESP_LOGI(TAG, "Record closed: %lu bytes, %s", (unsigned long)bytes, esp_err_to_name(err));
    return ok ? err : ESP_FAIL;
}

static esp_err_t play(const char *path, int16_t *buffer)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t header[44];
    struct stat info;
    bool valid = fread(header, 1, 44, file) == 44 && valid_header(header) &&
                 fstat(fileno(file), &info) == 0 && info.st_size == (off_t)get32(header + 40) + 44 &&
                 get32(header + 40) <= MAX_BYTES && (get32(header + 40) & 1) == 0;
    esp_err_t err = valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    bool opened = false;
    int volume = voice_memo_volume();
    uint32_t bytes = 0, total = valid ? get32(header + 40) : 0;
    if (valid && !stopped())
    {
        err = audio_start(RATE, 1);
        opened = err == ESP_OK;
        if (opened)
        {
            err = audio_set_volume(volume);
        }
    }
    while (opened && err == ESP_OK && !stopped() && bytes < total)
    {
        int requested = voice_memo_volume();
        if (requested != volume)
        {
            err = audio_set_volume(requested);
            volume = requested;
            if (err != ESP_OK)
            {
                break;
            }
        }
        size_t wanted = total - bytes < CHUNK * 2 ? (total - bytes) / 2 : CHUNK;
        size_t count = fread(buffer, 2, wanted, file);
        if (count != wanted)
        {
            err = ESP_FAIL;
            break;
        }
        levels(buffer, count);
        err = audio_write(buffer, count);
        bytes += count * 2;
        publish(VOICE_MEMO_PLAYING, bytes, total);
    }
    if (opened)
    {
        if (!stopped())
        {
            /* At 16 kHz the DMA queue can contain 180 ms, not 60 ms.
             * Keep checking alarm/exit requests while draining the tail. */
            for (int i = 0; i < 10 && !stopped(); i++)
            {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        audio_stop();
    }
    fclose(file);
    return err;
}

static void worker(void *arg)
{
    command_t *cmd = arg;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    const char *root = storage_dir();
    char dir[64], path[128];
    int16_t *buffer = NULL;
    power_keep_screen_on(true);
    if (root != NULL && sdcard_is_mounted())
    {
        snprintf(dir, sizeof(dir), "%s/memos", root);
        if (mkdir(dir, 0700) == 0 || errno == EEXIST)
        {
            err = ESP_OK;
            if (cmd->operation == RECORD || cmd->operation == PLAY)
            {
                buffer = heap_caps_malloc(CHUNK * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                err = buffer != NULL ? ESP_OK : ESP_ERR_NO_MEM;
            }
            if (err == ESP_OK)
            {
                snprintf(path, sizeof(path), "%s/%s", dir, cmd->name);
                if (cmd->operation == RECORD)
                {
                    err = record(dir, buffer);
                }
                else if (cmd->operation == PLAY)
                {
                    err = play(path, buffer);
                }
                else if (cmd->operation == DELETE)
                {
                    err = remove(path) == 0 ? ESP_OK : ESP_FAIL;
                }
                if (err == ESP_OK || cmd->operation == RECORD)
                {
                    esp_err_t scan_err = scan(dir);
                    if (err == ESP_OK)
                    {
                        err = scan_err;
                    }
                }
            }
        }
        else
        {
            err = ESP_FAIL;
        }
    }
    free(buffer);
    free(cmd);
    power_keep_screen_on(false);
    portENTER_CRITICAL(&s_guard);
    s_state.error = err;
    s_state.phase = err == ESP_OK ? VOICE_MEMO_IDLE : VOICE_MEMO_ERROR;
    portEXIT_CRITICAL(&s_guard);
    sys_heap_log("memo-closed");
    storage_files_release();
    __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
    vTaskDelete(NULL);
}

static bool start(operation_t operation, int index)
{
    if (!microphone_supported() || !sdcard_is_mounted())
    {
        return false;
    }
    if (!storage_files_acquire()) return false;
    bool expected = false;
    if (!__atomic_compare_exchange_n(&s_busy, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        storage_files_release();
        return false;
    }
    command_t *cmd = calloc(1, sizeof(*cmd));
    bool valid = cmd != NULL;
    portENTER_CRITICAL(&s_guard);
    if (valid && (operation == PLAY || operation == DELETE))
    {
        valid = index >= 0 && index < s_state.count;
        if (valid)
        {
            memcpy(cmd->name, s_state.names[index], sizeof(cmd->name));
        }
    }
    if (valid)
    {
        s_state.error = ESP_OK;
        s_state.elapsed_ms = 0;
        s_state.peak = 0;
        s_state.rms = 0;
        s_state.clipped = 0;
        memset(s_state.levels, 0, sizeof(s_state.levels));
        s_state.phase = operation == RECORD ? VOICE_MEMO_STARTING : VOICE_MEMO_SCANNING;
    }
    portEXIT_CRITICAL(&s_guard);
    if (valid)
    {
        cmd->operation = operation;
        __atomic_store_n(&s_stop, false, __ATOMIC_RELEASE);
        if (sys_task_create(worker, "voice_memo", 4096, cmd, 5, 1))
        {
            return true;
        }
    }
    free(cmd);
    portENTER_CRITICAL(&s_guard);
    s_state.phase = VOICE_MEMO_ERROR;
    s_state.error = valid ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    portEXIT_CRITICAL(&s_guard);
    storage_files_release();
    __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
    return false;
}

bool voice_memo_record(void) { return start(RECORD, 0); }
bool voice_memo_play(int index) { return start(PLAY, index); }
bool voice_memo_delete(int index) { return start(DELETE, index); }
bool voice_memo_scan(void) { return start(SCAN, 0); }
