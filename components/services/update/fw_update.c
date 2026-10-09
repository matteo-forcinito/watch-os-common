#include "services/fw_update.h"

#include "core/power.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/sys.h"
#include "services/extapp.h"
#include "services/fw_version.h"
#include "services/http_stream.h"
#include "services/wifi.h"
#include "hardware/sdcard.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "fw_update";

/* Image header + first segment header + app description: enough to check the file. */
#define HEAD_SIZE      (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
#define CHUNK          8192
#define RESTART_DELAY_US 2000000
#define CATALOG_MAX    (24 * 1024)     /* JSON text of the release catalog (PSRAM) */
#define COPY_CHUNK     (16 * 1024)
#define INCOMING_FILE  FW_SD_DIR "/.incoming.tmp"

/* Where the image being installed comes from: decides what is saved on the SD card. */
typedef enum
{
    SOURCE_PC,       /* web page upload */
    SOURCE_ONLINE,   /* download */
    SOURCE_SD,       /* a file on the card: nothing to save */
} source_t;

static atomic_bool s_busy = false;
static fw_update_phase_t s_phase = FW_UPDATE_IDLE;
static char s_message[64] = "";
static char s_new_version[32] = "";

/* current install */
static const esp_partition_t *s_slot = NULL;
static esp_ota_handle_t s_ota = 0;
static bool s_ota_open = false;
static size_t s_total = 0;
static size_t s_written = 0;
static int s_percent = -1;
static uint8_t s_head[HEAD_SIZE];
static size_t s_head_len = 0;
static esp_timer_handle_t s_restart_timer = NULL;
static source_t s_source = SOURCE_PC;
static FILE *s_copy = NULL;          /* the new image, also written to the SD card */
static char s_new_file_version[32] = "";

/* ------------------------------------------------------------ status */

static void set_phase(fw_update_phase_t phase, const char *message)
{
    s_phase = phase;

    if (message != NULL)
    {
        snprintf(s_message, sizeof(s_message), "%s", message);
    }

    state_set(STATE_UPDATE, phase);
}

static void set_percent(int percent)
{
    if (percent != s_percent)
    {
        s_percent = percent;
        state_set(STATE_UPDATE_PERCENT, percent);   /* whole percents only: ~100 updates */
    }
}

const char *fw_update_manifest_url(void)
{
    const char *custom = settings_get()->update_url;

    /* The user's address wins; otherwise the one this firmware was released with. */
    return custom[0] != '\0' ? custom : CONFIG_WATCH_UPDATE_MANIFEST_URL;
}

const char *fw_update_running_version(void)
{
    return esp_app_get_description()->version;
}

fw_update_phase_t fw_update_phase(void)
{
    return s_phase;
}

int fw_update_percent(void)
{
    return s_percent < 0 ? 0 : s_percent;
}

const char *fw_update_message(void)
{
    return s_message;
}

const char *fw_update_new_version(void)
{
    return s_new_version;
}

/* ------------------------------------------------------ SD card copies */

/* /sdcard/firmware/<project>-<version>.bin */
static void sd_file_name(const char *version, char *out, size_t size)
{
    snprintf(out, size, FW_SD_DIR "/%.24s-%.31s.bin", esp_app_get_description()->project_name, version);
}

static bool sd_ready(void)
{
    if (!sdcard_is_mounted())
    {
        return false;
    }

    mkdir(FW_SD_DIR, 0775);   /* fine if it exists */
    return true;
}

/*
 * Save the running firmware on the card (once per version) before it is
 * replaced: the way back without network. Best effort: a missing or full
 * card never stops an update.
 */
static void backup_running(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    char path[96];
    struct stat st;

    if (running == NULL || !sd_ready())
    {
        return;
    }

    sd_file_name(fw_update_running_version(), path, sizeof(path));

    if (stat(path, &st) == 0)
    {
        return;   /* already saved */
    }

    esp_partition_pos_t pos = {.offset = running->address, .size = running->size};
    esp_image_metadata_t meta = {0};

    if (esp_image_get_metadata(&pos, &meta) != ESP_OK || meta.image_len == 0)
    {
        return;
    }

    uint8_t *buffer = heap_caps_malloc(COPY_CHUNK, MALLOC_CAP_SPIRAM);
    FILE *file = buffer != NULL ? fopen(INCOMING_FILE, "wb") : NULL;
    bool ok = file != NULL;

    for (uint32_t done = 0; ok && done < meta.image_len; done += COPY_CHUNK)
    {
        uint32_t take = meta.image_len - done < COPY_CHUNK ? meta.image_len - done : COPY_CHUNK;
        ok = esp_partition_read(running, done, buffer, take) == ESP_OK && fwrite(buffer, 1, take, file) == take;
    }

    if (file != NULL)
    {
        fclose(file);
    }

    free(buffer);

    if (ok && rename(INCOMING_FILE, path) == 0)
    {
        ESP_LOGI(TAG, "Running firmware saved: %s", path);
    }
    else
    {
        unlink(INCOMING_FILE);
        ESP_LOGW(TAG, "Could not save the running firmware on the SD card");
    }
}

static void copy_open(void)
{
    s_copy = (s_source != SOURCE_SD && sd_ready()) ? fopen(INCOMING_FILE, "wb") : NULL;
}

static void copy_write(const void *data, size_t length)
{
    if (s_copy != NULL && fwrite(data, 1, length, s_copy) != length)
    {
        fclose(s_copy);   /* card full: drop the copy, keep installing */
        s_copy = NULL;
        unlink(INCOMING_FILE);
    }
}

static void copy_close(bool keep)
{
    if (s_copy == NULL)
    {
        return;
    }

    fclose(s_copy);
    s_copy = NULL;

    char path[96];
    sd_file_name(s_new_file_version, path, sizeof(path));

    if (keep && s_new_file_version[0] != '\0')
    {
        unlink(path);   /* same version saved before: replace it */

        if (rename(INCOMING_FILE, path) == 0)
        {
            ESP_LOGI(TAG, "New firmware saved: %s", path);
            return;
        }
    }

    unlink(INCOMING_FILE);
}

/* ----------------------------------------------------------- install */

static esp_err_t begin(size_t total_size);

static void restart_job(void *arg)
{
    (void)arg;
    power_restart();
}

static void restart_timer_cb(void *arg)
{
    (void)arg;
    sys_post(restart_job, NULL);   /* never real work in an esp_timer callback */
}

static void release(void)
{
    power_cpu_boost(false);
    power_keep_screen_on(false);
    wifi_set_fast(false);
    atomic_store(&s_busy, false);
}

void fw_update_abort(const char *reason)
{
    if (!atomic_load(&s_busy) || s_phase != FW_UPDATE_WRITING)
    {
        return;
    }

    if (s_ota_open)
    {
        esp_ota_abort(s_ota);
        s_ota_open = false;
    }

    copy_close(false);
    ESP_LOGE(TAG, "Update stopped: %s", reason);
    set_phase(FW_UPDATE_FAILED, reason);
    release();
}

/* Install from the PC (web page): the image arrives by fw_update_write(). */
esp_err_t fw_update_begin(size_t total_size)
{
    s_source = SOURCE_PC;
    return begin(total_size);
}

static esp_err_t begin(size_t total_size)
{
    bool expected = false;

    if (!atomic_compare_exchange_strong(&s_busy, &expected, true))
    {
        return ESP_ERR_INVALID_STATE;   /* another update is running */
    }

    s_slot = esp_ota_get_next_update_partition(NULL);

    if (s_slot == NULL || total_size < HEAD_SIZE || total_size > s_slot->size)
    {
        set_phase(FW_UPDATE_FAILED, s_slot == NULL ? "Nessuno slot libero" : "Dimensione del file non valida");
        atomic_store(&s_busy, false);
        return ESP_ERR_INVALID_SIZE;
    }

    s_total = total_size;
    s_written = 0;
    s_head_len = 0;
    s_ota_open = false;
    s_percent = -1;

    power_cpu_boost(true);
    power_keep_screen_on(true);
    wifi_set_fast(true);

    set_percent(0);
    set_phase(FW_UPDATE_WRITING, "");

    if (s_source != SOURCE_SD)
    {
        backup_running();   /* the way back, on the SD card (a few seconds) */
    }

    s_new_file_version[0] = '\0';
    copy_open();
    ESP_LOGI(TAG, "Receiving %u KB into %s", (unsigned)(total_size / 1024), s_slot->label);

    return ESP_OK;
}

/* The first bytes are checked before the slot is erased. */
static esp_err_t check_head(void)
{
    const esp_image_header_t *image = (const esp_image_header_t *)s_head;
    const esp_app_desc_t *desc =
        (const esp_app_desc_t *)(s_head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    const esp_app_desc_t *running = esp_app_get_description();

    if (image->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD)
    {
        fw_update_abort("Non è un firmware (usa il .bin nella cartella build)");
        return ESP_ERR_INVALID_ARG;
    }

    if (image->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID)
    {
        fw_update_abort("Firmware per un altro chip");
        return ESP_ERR_INVALID_VERSION;
    }

    if (strncmp(desc->project_name, running->project_name, sizeof(desc->project_name)) != 0)
    {
        fw_update_abort("Non è il firmware di sistema");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "New firmware %.32s (running %s)", desc->version, running->version);
    snprintf(s_new_file_version, sizeof(s_new_file_version), "%.31s", desc->version);
    snprintf(s_new_version, sizeof(s_new_version), "%.31s", desc->version);

    /* From here the app slot changes: no external app is cached in it any more. */
    extapp_forget_cache();

    esp_err_t err = esp_ota_begin(s_slot, OTA_WITH_SEQUENTIAL_WRITES, &s_ota);

    if (err != ESP_OK)
    {
        fw_update_abort("Flash non scrivibile");
        return err;
    }

    s_ota_open = true;
    err = esp_ota_write(s_ota, s_head, s_head_len);

    if (err != ESP_OK)
    {
        fw_update_abort("Errore di scrittura");
    }

    return err;
}

esp_err_t fw_update_write(const void *data, size_t length)
{
    if (!atomic_load(&s_busy) || s_phase != FW_UPDATE_WRITING)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t *bytes = data;

    if (s_written + length > s_total)
    {
        fw_update_abort("Ricevuti più dati del previsto");
        return ESP_ERR_INVALID_SIZE;
    }

    s_written += length;
    copy_write(data, length);

    if (!s_ota_open)
    {
        size_t take = HEAD_SIZE - s_head_len;
        take = take < length ? take : length;
        memcpy(s_head + s_head_len, bytes, take);
        s_head_len += take;
        bytes += take;
        length -= take;

        if (s_head_len < HEAD_SIZE)
        {
            return ESP_OK;
        }

        esp_err_t err = check_head();

        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (length > 0)
    {
        esp_err_t err = esp_ota_write(s_ota, bytes, length);

        if (err != ESP_OK)
        {
            fw_update_abort("Errore di scrittura");
            return err;
        }
    }

    int percent = (int)((uint64_t)s_written * 100 / s_total);
    set_percent(percent >= 100 ? 99 : percent);   /* 100 only once verified */

    return ESP_OK;
}

esp_err_t fw_update_end(void)
{
    if (!atomic_load(&s_busy) || s_phase != FW_UPDATE_WRITING)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_ota_open || s_written != s_total)
    {
        fw_update_abort("Trasferimento incompleto");
        return ESP_ERR_INVALID_SIZE;
    }

    s_ota_open = false;
    esp_err_t err = esp_ota_end(s_ota);   /* SHA-256 of the whole image */

    if (err == ESP_OK)
    {
        err = esp_ota_set_boot_partition(s_slot);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Image rejected: %s", esp_err_to_name(err));
        copy_close(false);
        set_phase(FW_UPDATE_FAILED, "Firmware danneggiato, riprova");
        release();
        return err;
    }

    copy_close(true);   /* verified: keep it on the card */

    set_percent(100);
    set_phase(FW_UPDATE_RESTARTING, "");
    ESP_LOGI(TAG, "Firmware installed in %s, restarting", s_slot->label);

    if (s_restart_timer == NULL)
    {
        const esp_timer_create_args_t args = {.callback = restart_timer_cb, .name = "fw_restart"};
        esp_timer_create(&args, &s_restart_timer);
    }

    /* a moment for the web page answer and the "restarting" screen */
    esp_timer_start_once(s_restart_timer, RESTART_DELAY_US);

    return ESP_OK;
}

/* ------------------------------------------------------------ online */

static void download(const char *url)
{
    http_stream_t stream;
    uint8_t *buffer = malloc(CHUNK);

    if (buffer == NULL || http_stream_open(&stream, url, false) != ESP_OK)
    {
        free(buffer);
        set_phase(FW_UPDATE_FAILED, "Download non riuscito");
        return;
    }

    int64_t length = esp_http_client_get_content_length(stream.client);

    s_source = SOURCE_ONLINE;

    if (length <= 0 || begin((size_t)length) != ESP_OK)
    {
        if (s_phase != FW_UPDATE_FAILED)
        {
            set_phase(FW_UPDATE_FAILED, "Dimensione del file sconosciuta");
        }

        http_stream_close(&stream);
        free(buffer);
        return;
    }

    esp_err_t err = ESP_OK;
    int got;

    while (err == ESP_OK && (got = http_stream_read(&stream, buffer, CHUNK)) > 0)
    {
        err = fw_update_write(buffer, (size_t)got);
    }

    http_stream_close(&stream);
    free(buffer);

    if (err == ESP_OK)
    {
        fw_update_end();   /* reports an incomplete download by itself */
    }
}

/* The catalog text (PSRAM), or NULL. Wi-Fi must be held by the caller. */
static char *fetch_catalog(void)
{
    char *text = heap_caps_malloc(CATALOG_MAX, MALLOC_CAP_SPIRAM);

    if (text != NULL && http_stream_get_text(fw_update_manifest_url(), text, CATALOG_MAX, NULL, 0) != ESP_OK)
    {
        free(text);
        text = NULL;
    }

    return text;
}

static void copy_string(const cJSON *json, const char *key, char *out, size_t size)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(json, key));
    snprintf(out, size, "%s", value != NULL ? value : "");
}

/*
 * The version to offer: the chosen channel's newest. On "dev" a stable
 * release newer than the last dev build wins too. Old catalogs (only
 * "version" + "url") are read as the stable channel.
 */
static bool pick_candidate(const cJSON *json, char *version, size_t version_size, char *url, size_t url_size)
{
    const cJSON *channels = cJSON_GetObjectItem(json, "channels");
    const cJSON *stable = channels != NULL ? cJSON_GetObjectItem(channels, "stable") : json;
    const cJSON *dev = channels != NULL ? cJSON_GetObjectItem(channels, "dev") : NULL;
    const cJSON *pick = stable;

    const char *stable_version = cJSON_GetStringValue(cJSON_GetObjectItem(stable, "version"));
    const char *dev_version = cJSON_GetStringValue(cJSON_GetObjectItem(dev, "version"));

    if (settings_get()->update_channel == FW_CHANNEL_DEV && dev_version != NULL &&
        (stable_version == NULL || fw_version_compare(dev_version, stable_version) > 0))
    {
        pick = dev;
    }

    copy_string(pick, "version", version, version_size);
    copy_string(pick, "url", url, url_size);

    return version[0] != '\0' && url[0] != '\0';
}

static void online_task(void *arg)
{
    bool install = (bool)(intptr_t)arg;
    char *bin_url = malloc(HTTP_STREAM_URL_MAX);
    char *text = NULL;
    cJSON *json = NULL;

    if (bin_url == NULL)
    {
        set_phase(FW_UPDATE_FAILED, "Memoria insufficiente");
    }
    else if (wifi_acquire(15000) != ESP_OK)
    {
        set_phase(FW_UPDATE_FAILED, "Wi-Fi non disponibile");
    }
    else
    {
        text = fetch_catalog();
        json = text != NULL ? cJSON_Parse(text) : NULL;

        if (json == NULL || !pick_candidate(json, s_new_version, sizeof(s_new_version), bin_url, HTTP_STREAM_URL_MAX))
        {
            set_phase(FW_UPDATE_FAILED, "Elenco versioni non leggibile");
        }
        else if (fw_version_compare(s_new_version, fw_update_running_version()) <= 0)
        {
            set_phase(FW_UPDATE_UP_TO_DATE, "");
        }
        else if (!install)
        {
            set_phase(FW_UPDATE_AVAILABLE, "");
        }
        else
        {
            download(bin_url);
        }

        wifi_release();
    }

    cJSON_Delete(json);
    free(text);
    free(bin_url);
    vTaskDelete(NULL);
}

void fw_update_check_online(bool install)
{
    if (atomic_load(&s_busy) || s_phase == FW_UPDATE_CHECKING)
    {
        return;
    }

    if (fw_update_manifest_url()[0] == '\0')
    {
        set_phase(FW_UPDATE_FAILED, "Indirizzo aggiornamenti non impostato");
        return;
    }

    set_phase(FW_UPDATE_CHECKING, "");

    /* TLS needs a deep stack */
    if (!sys_task_create(online_task, "fw_online", 8192, (void *)(intptr_t)install, 4, SYS_CORE_ANY))
    {
        set_phase(FW_UPDATE_FAILED, "Memoria insufficiente");
    }
}

/* -------------------------------------------------------- every release */

static int newest_first(const void *a, const void *b)
{
    return fw_version_compare(((const fw_release_t *)b)->version, ((const fw_release_t *)a)->version);
}

esp_err_t fw_update_fetch_releases(fw_release_t *out, int max, int *count)
{
    *count = 0;

    if (fw_update_manifest_url()[0] == '\0')
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (wifi_acquire(15000) != ESP_OK)
    {
        return ESP_ERR_TIMEOUT;
    }

    char *text = fetch_catalog();
    wifi_release();

    cJSON *json = text != NULL ? cJSON_Parse(text) : NULL;
    const cJSON *item = NULL;

    cJSON_ArrayForEach(item, cJSON_GetObjectItem(json, "releases"))
    {
        if (*count >= max)
        {
            break;
        }

        fw_release_t *r = &out[*count];
        copy_string(item, "version", r->version, sizeof(r->version));
        copy_string(item, "channel", r->channel, sizeof(r->channel));
        copy_string(item, "date", r->date, sizeof(r->date));
        copy_string(item, "notes", r->notes, sizeof(r->notes));
        copy_string(item, "url", r->url, sizeof(r->url));
        r->size = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "size"));

        if (r->version[0] != '\0' && r->url[0] != '\0')
        {
            (*count)++;
        }
    }

    esp_err_t err = json != NULL ? ESP_OK : ESP_ERR_INVALID_RESPONSE;

    cJSON_Delete(json);
    free(text);

    qsort(out, (size_t)*count, sizeof(fw_release_t), newest_first);

    return err;
}

static void release_task(void *arg)
{
    char *url = arg;

    if (wifi_acquire(15000) != ESP_OK)
    {
        set_phase(FW_UPDATE_FAILED, "Wi-Fi non disponibile");
    }
    else
    {
        download(url);
        wifi_release();
    }

    free(url);
    vTaskDelete(NULL);
}

void fw_update_install_release(const fw_release_t *release)
{
    char *url = strdup(release->url);

    if (url == NULL || atomic_load(&s_busy))
    {
        free(url);
        return;
    }

    snprintf(s_new_version, sizeof(s_new_version), "%s", release->version);
    set_phase(FW_UPDATE_CHECKING, "");

    if (!sys_task_create(release_task, "fw_release", 8192, url, 4, SYS_CORE_ANY))
    {
        free(url);
        set_phase(FW_UPDATE_FAILED, "Memoria insufficiente");
    }
}

/* ------------------------------------------------------------ SD card */

/* Version inside a firmware file, if it is a firmware for this device. */
static bool read_file_version(const char *path, char *version, size_t size)
{
    uint8_t head[HEAD_SIZE];
    FILE *file = fopen(path, "rb");
    bool ok = file != NULL && fread(head, 1, sizeof(head), file) == sizeof(head);

    if (file != NULL)
    {
        fclose(file);
    }

    if (!ok)
    {
        return false;
    }

    const esp_image_header_t *image = (const esp_image_header_t *)head;
    const esp_app_desc_t *desc =
        (const esp_app_desc_t *)(head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));

    if (image->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD ||
        image->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID ||
        strncmp(desc->project_name, esp_app_get_description()->project_name, sizeof(desc->project_name)) != 0)
    {
        return false;
    }

    snprintf(version, size, "%.31s", desc->version);
    return true;
}

static int local_newest_first(const void *a, const void *b)
{
    return fw_version_compare(((const fw_local_t *)b)->version, ((const fw_local_t *)a)->version);
}

int fw_update_list_sd(fw_local_t *out, int max)
{
    int count = 0;
    DIR *dir = sdcard_is_mounted() ? opendir(FW_SD_DIR) : NULL;

    if (dir == NULL)
    {
        return 0;
    }

    struct dirent *entry;

    while (count < max && (entry = readdir(dir)) != NULL)
    {
        size_t length = strlen(entry->d_name);

        if (entry->d_name[0] == '.' || length < 5 || strcasecmp(entry->d_name + length - 4, ".bin") != 0)
        {
            continue;
        }

        snprintf(out[count].path, sizeof(out[count].path), FW_SD_DIR "/%.80s", entry->d_name);

        if (read_file_version(out[count].path, out[count].version, sizeof(out[count].version)))
        {
            count++;
        }
    }

    closedir(dir);
    qsort(out, (size_t)count, sizeof(fw_local_t), local_newest_first);

    return count;
}

static void file_task(void *arg)
{
    char *path = arg;
    uint8_t *buffer = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    FILE *file = buffer != NULL ? fopen(path, "rb") : NULL;
    struct stat st;

    s_source = SOURCE_SD;

    if (file == NULL || stat(path, &st) != 0 || begin((size_t)st.st_size) != ESP_OK)
    {
        if (s_phase != FW_UPDATE_FAILED)
        {
            set_phase(FW_UPDATE_FAILED, "File non leggibile");
        }
    }
    else
    {
        esp_err_t err = ESP_OK;
        size_t got;

        while (err == ESP_OK && (got = fread(buffer, 1, CHUNK, file)) > 0)
        {
            err = fw_update_write(buffer, got);
        }

        if (err == ESP_OK)
        {
            fw_update_end();
        }
    }

    if (file != NULL)
    {
        fclose(file);
    }

    free(buffer);
    free(path);
    vTaskDelete(NULL);
}

void fw_update_install_file(const char *path)
{
    char *copy = strdup(path);

    if (copy == NULL || atomic_load(&s_busy))
    {
        free(copy);
        return;
    }

    set_phase(FW_UPDATE_CHECKING, "");

    if (!sys_task_create(file_task, "fw_file", 4096, copy, 4, SYS_CORE_ANY))
    {
        free(copy);
        set_phase(FW_UPDATE_FAILED, "Memoria insufficiente");
    }
}

void fw_update_confirm_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY)
    {
        ESP_LOGI(TAG, "Firmware %s in %s confirmed", fw_update_running_version(), running->label);
    }

    esp_ota_mark_app_valid_cancel_rollback();
}
