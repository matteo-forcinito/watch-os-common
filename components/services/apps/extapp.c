#include "services/extapp.h"

#include "core/power.h"
#include "hardware/sdcard.h"

#include "cJSON.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "extapp";

#define APPS_DIR      SDCARD_MOUNT "/apps"
#define CHUNK         (16 * 1024)
#define NVS_NAMESPACE "extapp"

typedef struct
{
    char id[32];
    uint32_t size;
    int64_t mtime;
} cache_t;

static char s_error[48] = "";

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void read_manifest(const char *dir, extapp_t *app)
{
    char path[EXTAPP_PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/manifest.json", dir);

    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        return;
    }

    char text[512] = {0};
    fread(text, 1, sizeof(text) - 1, file);
    fclose(file);

    cJSON *root = cJSON_Parse(text);
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    const char *bin = cJSON_GetStringValue(cJSON_GetObjectItem(root, "bin"));

    if (name != NULL)
    {
        snprintf(app->name, sizeof(app->name), "%s", name);
    }

    if (bin != NULL)
    {
        snprintf(app->bin, sizeof(app->bin), "%s/%s", dir, bin);
    }

    cJSON_Delete(root);
}

int extapp_scan(extapp_t *out, int max)
{
    DIR *dir = sdcard_is_mounted() ? opendir(APPS_DIR) : NULL;
    int count = 0;

    if (dir == NULL)
    {
        return 0;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && count < max)
    {
        if (entry->d_type != DT_DIR || entry->d_name[0] == '.')
        {
            continue;
        }

        extapp_t *app = &out[count];
        memset(app, 0, sizeof(*app));
        snprintf(app->id, sizeof(app->id), "%.31s", entry->d_name);
        snprintf(app->name, sizeof(app->name), "%.31s", entry->d_name);

        char folder[64];
        snprintf(folder, sizeof(folder), APPS_DIR "/%.40s", entry->d_name);
        snprintf(app->bin, sizeof(app->bin), "%s/%.40s.bin", folder, entry->d_name);
        read_manifest(folder, app);

        struct stat st;

        if (stat(app->bin, &st) != 0)
        {
            continue;   /* not an app folder */
        }

        app->size = (size_t)st.st_size;

        char icon[EXTAPP_PATH_MAX + 16];
        snprintf(icon, sizeof(icon), "%s/icon.png", folder);

        if (!file_exists(icon))
        {
            snprintf(icon, sizeof(icon), "%s/icon.bin", folder);
        }

        if (file_exists(icon))
        {
            /* LVGL path: drive S: is /sdcard */
            snprintf(app->icon, sizeof(app->icon), "S:%.180s", icon + strlen(SDCARD_MOUNT));
        }

        count++;
    }

    closedir(dir);
    ESP_LOGI(TAG, "%d apps on the SD card", count);

    return count;
}

static bool current_stamp(const extapp_t *app, cache_t *stamp)
{
    struct stat st;

    if (stat(app->bin, &st) != 0)
    {
        return false;
    }

    memset(stamp, 0, sizeof(*stamp));
    snprintf(stamp->id, sizeof(stamp->id), "%s", app->id);
    stamp->size = (uint32_t)st.st_size;
    stamp->mtime = (int64_t)st.st_mtime;

    return true;
}

bool extapp_is_cached(const extapp_t *app)
{
    cache_t now;
    cache_t saved = {0};
    size_t size = sizeof(saved);
    nvs_handle_t nvs;

    if (!current_stamp(app, &now) || nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
    {
        return false;
    }

    esp_err_t err = nvs_get_blob(nvs, "slot", &saved, &size);
    nvs_close(nvs);

    if (err != ESP_OK || memcmp(&saved, &now, sizeof(now)) != 0)
    {
        return false;
    }

    /* The slot must still hold a valid image (not erased by a USB flash). */
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t desc;

    return slot != NULL && esp_ota_get_partition_description(slot, &desc) == ESP_OK;
}

static esp_err_t fail(const char *message, esp_err_t err)
{
    snprintf(s_error, sizeof(s_error), "%s", message);
    ESP_LOGE(TAG, "%s (%s)", message, esp_err_to_name(err));
    return err;
}

static void boot_into(const esp_partition_t *slot)
{
    esp_err_t err = esp_ota_set_boot_partition(slot);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Starting app from %s", slot->label);
        power_restart();
    }
}

/*
 * Where the app image starts in the file. Arduino "Export compiled binary"
 * also writes a *merged* file (bootloader at 0, partition table at 0x8000,
 * app at 0x10000): accept both.
 */
static long find_app_offset(FILE *file, long size)
{
    uint8_t probe[4];

    fseek(file, 0x8000, SEEK_SET);

    if (size > 0x10000 + (long)sizeof(esp_image_header_t) && fread(probe, 1, 2, file) == 2 &&
        probe[0] == 0xAA && probe[1] == 0x50)   /* partition table magic */
    {
        ESP_LOGI(TAG, "Merged image: app at 0x10000");
        return 0x10000;
    }

    return 0;
}

void extapp_forget_cache(void)
{
    nvs_handle_t nvs;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_erase_key(nvs, "slot");
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

esp_err_t extapp_launch(const extapp_t *app, void (*progress)(int percent, void *ctx), void *ctx)
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);

    s_error[0] = '\0';

    if (slot == NULL)
    {
        return fail("Nessuno slot app", ESP_ERR_NOT_FOUND);
    }

    if (extapp_is_cached(app))
    {
        if (progress != NULL)
        {
            progress(100, ctx);
        }

        boot_into(slot);
        return fail("Avvio fallito", ESP_FAIL);
    }

    FILE *file = fopen(app->bin, "rb");

    if (file == NULL)
    {
        return fail("File .bin non trovato", ESP_ERR_NOT_FOUND);
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    long offset = find_app_offset(file, size);
    long image_size = size - offset;

    if (image_size < (long)sizeof(esp_image_header_t) || (size_t)image_size > slot->size)
    {
        fclose(file);
        return fail(image_size > 0 ? "App troppo grande" : "File vuoto", ESP_ERR_INVALID_SIZE);
    }

    uint8_t *buffer = malloc(CHUNK);

    if (buffer == NULL)
    {
        fclose(file);
        return fail("Memoria insufficiente", ESP_ERR_NO_MEM);
    }

    /* Check it is an ESP32-S3 firmware before erasing anything. */
    fseek(file, offset, SEEK_SET);
    size_t got = fread(buffer, 1, CHUNK, file);
    const esp_image_header_t *header = (const esp_image_header_t *)buffer;

    if (got < sizeof(*header) || header->magic != ESP_IMAGE_HEADER_MAGIC)
    {
        free(buffer);
        fclose(file);
        return fail("Non e' un firmware ESP32", ESP_ERR_INVALID_VERSION);
    }

    if (header->chip_id != ESP_CHIP_ID_ESP32S3)
    {
        free(buffer);
        fclose(file);
        return fail("Firmware per un altro chip", ESP_ERR_INVALID_VERSION);
    }

    /* The slot is about to change: the old "already installed" note is wrong from now on. */
    extapp_forget_cache();

    /* Sequential writes: each sector is erased just before it is written, so the
       progress bar moves from the start instead of waiting for a long erase. */
    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &ota);

    if (err != ESP_OK)
    {
        free(buffer);
        fclose(file);
        return fail("Flash non scrivibile", err);
    }

    ESP_LOGI(TAG, "Installing %s (%ld KB) into %s", app->name, image_size / 1024, slot->label);

    power_cpu_boost(true);
    power_keep_screen_on(true);

    long written = 0;
    int last_percent = -1;

    while (got > 0)
    {
        err = esp_ota_write(ota, buffer, got);

        if (err != ESP_OK)
        {
            break;
        }

        written += (long)got;
        int percent = (int)(written * 100 / image_size);

        if (progress != NULL && percent != last_percent)
        {
            last_percent = percent;
            progress(percent >= 100 ? 99 : percent, ctx);
        }

        got = fread(buffer, 1, CHUNK, file);
    }

    free(buffer);
    fclose(file);
    power_cpu_boost(false);
    power_keep_screen_on(false);

    if (err != ESP_OK)
    {
        esp_ota_abort(ota);
        return fail("Errore di scrittura", err);
    }

    err = esp_ota_end(ota);   /* verifies the image (checksum / SHA) */

    if (err != ESP_OK)
    {
        return fail("Immagine non valida", err);
    }

    cache_t stamp;
    nvs_handle_t nvs;

    if (current_stamp(app, &stamp) && nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_blob(nvs, "slot", &stamp, sizeof(stamp));
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    if (progress != NULL)
    {
        progress(100, ctx);
    }

    boot_into(slot);

    return fail("Avvio fallito", ESP_FAIL);
}

const char *extapp_last_error(void)
{
    return s_error;
}
