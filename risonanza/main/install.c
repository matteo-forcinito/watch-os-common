/* Publish the running USB test image in the SD launcher, without touching other apps. */
#include "install.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "risonanza_install";
static int s_state;
int risonanza_launcher_state(void)
{
    return __atomic_load_n(&s_state, __ATOMIC_ACQUIRE);
}

/* Existing launcher slotapp ABI, shared by SD-less targets too. */
typedef struct
{
    char id[32], name[32], version[32], project[32], icon[96];
} slot_note_t;

void risonanza_install_launcher(void *arg)
{
    (void)arg;
    const esp_app_desc_t *desc = esp_app_get_description();
    slot_note_t note = {0}, saved;
    snprintf(note.id, sizeof(note.id), "Risonanza");
    snprintf(note.name, sizeof(note.name), "Risonanza");
    snprintf(note.version, sizeof(note.version), "%.31s", desc->version);
    snprintf(note.project, sizeof(note.project), "%.31s", desc->project_name);
    nvs_handle_t nvs;
    if (nvs_open("extapp", NVS_READWRITE, &nvs) == ESP_OK)
    {
        size_t length = sizeof(saved);
        if (nvs_get_blob(nvs, "slotapp", &saved, &length) != ESP_OK || length != sizeof(note) ||
            memcmp(&note, &saved, sizeof(note)))
        {
            if (nvs_set_blob(nvs, "slotapp", &note, sizeof(note)) == ESP_OK)
            {
                nvs_commit(nvs);
            }
        }
        nvs_close(nvs);
    }
    struct stat st;
    if (stat("/sdcard", &st) != 0)
    {
        return;
    }
    mkdir("/sdcard/apps", 0775);
    mkdir("/sdcard/apps/Risonanza", 0775);
    const char *image = "/sdcard/apps/Risonanza/Risonanza.bin";
    const char *pending = "/sdcard/apps/Risonanza/.risonanza-pending";
    const char *previous = "/sdcard/apps/Risonanza/.previous";
    const esp_partition_t *part = esp_ota_get_running_partition();
    esp_image_metadata_t meta;
    esp_partition_pos_t pos = {.offset = part->address, .size = part->size};
    if (esp_image_get_metadata(&pos, &meta) != ESP_OK)
    {
        return;
    }
    unsigned char *buffer = malloc(16384);
    unsigned char *compare = malloc(16384);
    if (!buffer || !compare)
    {
        free(buffer);
        free(compare);
        return;
    }
    FILE *file = fopen(image, "rb");
    bool identical = false;
    if (file)
    {
        unsigned char header[112] = {0};
        bool ours = fread(header, 1, sizeof(header), file) == sizeof(header) &&
                    !memcmp(header + 80, desc->project_name, 32);
        if (!ours)
        {
            ESP_LOGW(TAG, "Risonanza folder belongs to a different image; preserving it");
            fclose(file);
            free(buffer);
            free(compare);
            return;
        }
        rewind(file);
        identical = stat(image, &st) == 0 && (size_t)st.st_size == meta.image_len;
        for (size_t offset = 0; identical && offset < meta.image_len; offset += 16384)
        {
            size_t length = meta.image_len - offset < 16384 ? meta.image_len - offset : 16384;
            identical = esp_partition_read(part, offset, buffer, length) == ESP_OK &&
                        fread(compare, 1, length, file) == length && !memcmp(buffer, compare, length);
        }
        fclose(file);
    }
    bool ok = true;
    if (!identical)
    {
        file = fopen(pending, "wb");
        ok = file != NULL;
        for (size_t offset = 0; ok && offset < meta.image_len; offset += 16384)
        {
            size_t length = meta.image_len - offset < 16384 ? meta.image_len - offset : 16384;
            ok = esp_partition_read(part, offset, buffer, length) == ESP_OK &&
                 fwrite(buffer, 1, length, file) == length;
        }
        if (file)
        {
            ok = fflush(file) == 0 && ok;
            ok = fsync(fileno(file)) == 0 && ok;
            ok = fclose(file) == 0 && ok;
        }
        if (ok && stat(image, &st) == 0)
        {
            remove(previous);
            ok = rename(image, previous) == 0;
        }
        if (ok)
        {
            ok = rename(pending, image) == 0;
        }
        if (!ok)
        {
            remove(pending);
        }
    }
    free(buffer);
    free(compare);
    if (!ok)
    {
        ESP_LOGW(TAG, "SD publication failed; USB slot is still usable");
        return;
    }
    file = fopen("/sdcard/apps/Risonanza/.manifest-pending", "w");
    if (!file)
    {
        return;
    }
    ok = fprintf(file, "{\"name\":\"Risonanza\",\"bin\":\"Risonanza.bin\",\"version\":\"%s\"}\n", desc->version) > 0;
    ok = fflush(file) == 0 && ok;
    ok = fsync(fileno(file)) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    if (ok)
    {
        remove("/sdcard/apps/Risonanza/manifest.json");
        ok = rename("/sdcard/apps/Risonanza/.manifest-pending", "/sdcard/apps/Risonanza/manifest.json") == 0;
    }
    if (ok)
    {
        __atomic_store_n(&s_state, 1, __ATOMIC_RELEASE);
        ESP_LOGI(TAG, "Risonanza available in SD launcher (%lu bytes)", (unsigned long)meta.image_len);
    }
}
