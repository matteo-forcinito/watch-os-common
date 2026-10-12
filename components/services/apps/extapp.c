#include "services/extapp.h"
#include "services/storage.h"

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
#include <unistd.h>

static const char *TAG = "extapp";

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

/* Manifest file names stay within the app folder. Older manually copied
 * applications without a manifest retain their original naming convention. */
static bool safe_leaf(const char *name)
{
    return name != NULL && name[0] != '\0' && strlen(name) < 64 &&
           strchr(name, '/') == NULL && strchr(name, '\\') == NULL &&
           strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static bool read_manifest(const char *dir, extapp_t *app)
{
    char path[EXTAPP_PATH_MAX + 24];
    snprintf(path, sizeof(path), "%s/manifest.json", dir);
    FILE *file = fopen(path, "r");
    if (file == NULL)
    {
        /* A reset between the two FAT renames leaves the previous manifest.
         * Read it in place: scanning must not race a publication worker. */
        snprintf(path, sizeof(path), "%s/.manifest.previous", dir);
        file = fopen(path, "r");
    }
    if (file == NULL) return true;
    char text[1024] = {0};
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    bool complete = !ferror(file) && fgetc(file) == EOF;
    fclose(file);
    if (!complete || memchr(text, '\0', length) != NULL) return true;
    cJSON *root = cJSON_Parse(text);
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "removed")))
    {
        cJSON_Delete(root);
        return false;
    }
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    const char *bin = cJSON_GetStringValue(cJSON_GetObjectItem(root, "bin"));
    const char *icon = cJSON_GetStringValue(cJSON_GetObjectItem(root, "icon"));
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(root, "version"));
    if (name != NULL) snprintf(app->name, sizeof(app->name), "%s", name);
    if (safe_leaf(bin)) snprintf(app->bin, sizeof(app->bin), "%s/%s", dir, bin);
    if (safe_leaf(icon)) snprintf(app->icon, sizeof(app->icon), "%s/%s", dir, icon);
    if (version != NULL) snprintf(app->version, sizeof(app->version), "%s", version);
    cJSON_Delete(root);
    return true;
}

/* Caller holds storage_files_acquire() while publishing an uploaded image. */
esp_err_t extapp_upload_manifest(const char *folder, const char *binary_name)
{
    if (folder == NULL || !safe_leaf(binary_name)) return ESP_ERR_INVALID_ARG;
    char path[EXTAPP_PATH_MAX + 32], pending[EXTAPP_PATH_MAX + 32];
    char previous[EXTAPP_PATH_MAX + 32], image[EXTAPP_PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/manifest.json", folder);
    snprintf(previous, sizeof(previous), "%s/.manifest.previous", folder);
    snprintf(pending, sizeof(pending), "%s/.upload-manifest", folder);
    snprintf(image, sizeof(image), "%s/%s", folder, binary_name);
    if (!file_exists(path) && file_exists(previous) && rename(previous, path) != 0) return ESP_FAIL;
    cJSON *root = NULL;
    FILE *file = fopen(path, "rb");
    if (file != NULL)
    {
        char text[1024] = {0};
        size_t length = fread(text, 1, sizeof(text) - 1, file);
        bool complete = !ferror(file) && fgetc(file) == EOF && memchr(text, '\0', length) == NULL;
        fclose(file);
        if (!complete) return ESP_FAIL;
        root = cJSON_Parse(text);
        if (!cJSON_IsObject(root))
        {
            cJSON_Delete(root);
            return ESP_FAIL;
        }
    }
    else root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    cJSON_DeleteItemFromObjectCaseSensitive(root, "bin");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "version");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "channel");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "removed");
    bool ok = cJSON_AddStringToObject(root, "bin", binary_name) != NULL;
    uint8_t header[288] = {0};
    file = fopen(image, "rb");
    if (file != NULL)
    {
        size_t length = fread(header, 1, sizeof(header), file);
        fclose(file);
        uint32_t magic = 0;
        memcpy(&magic, header + 32, sizeof(magic));
        if (length == sizeof(header) && header[0] == 0xE9 &&
            magic == ESP_APP_DESC_MAGIC_WORD && memchr(header + 48, '\0', 32) != NULL)
            ok = cJSON_AddStringToObject(root, "version", (const char *)header + 48) != NULL && ok;
    }
    char *text = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (text == NULL) return ESP_ERR_NO_MEM;
    file = fopen(pending, "wb");
    ok = file != NULL;
    if (ok)
    {
        ok = fwrite(text, 1, strlen(text), file) == strlen(text) &&
             fflush(file) == 0 && fsync(fileno(file)) == 0;
        if (fclose(file) != 0) ok = false;
    }
    free(text);
    if (!ok)
    {
        remove(pending);
        return ESP_FAIL;
    }
    if (file_exists(path))
    {
        if (file_exists(previous) && remove(previous) != 0) return ESP_FAIL;
        if (rename(path, previous) != 0) return ESP_FAIL;
    }
    if (rename(pending, path) != 0)
    {
        if (file_exists(previous)) rename(previous, path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

int extapp_scan(extapp_t *out, int max)
{
    const char *root = storage_dir();
    char apps_dir[32];
    snprintf(apps_dir, sizeof(apps_dir), "%s/apps", root != NULL ? root : "");
    DIR *dir = root != NULL ? opendir(apps_dir) : NULL;
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
        snprintf(folder, sizeof(folder), "%.16s/%.40s", apps_dir, entry->d_name);
        snprintf(app->bin, sizeof(app->bin), "%s/%.40s.bin", folder, entry->d_name);
        if (!read_manifest(folder, app)) continue;

        char previous[EXTAPP_PATH_MAX + 16];
        snprintf(previous, sizeof(previous), "%s/.previous", folder);
        if (!file_exists(app->bin) && file_exists(previous))
        {
            /* Recover the old complete image if power failed during publication. */
            rename(previous, app->bin);
        }

        struct stat st;

        if (stat(app->bin, &st) != 0)
        {
            continue;   /* not an app folder */
        }

        app->size = (size_t)st.st_size;

        char icon[EXTAPP_PATH_MAX + 16];
        if (app->icon[0] != '\0') snprintf(icon, sizeof(icon), "%s", app->icon);
        else snprintf(icon, sizeof(icon), "%s/icon.png", folder);
        app->icon[0] = '\0';

        if (!file_exists(icon))
        {
            snprintf(icon, sizeof(icon), "%s/icon.bin", folder);
        }

        if (file_exists(icon) && sdcard_is_mounted())
        {
            /* LVGL path: drive S: is /sdcard; internal-storage apps use a built-in icon. */
            snprintf(app->icon, sizeof(app->icon), "S:%.180s", icon + strlen(SDCARD_MOUNT));
        }

        count++;
    }

    closedir(dir);
    ESP_LOGI(TAG, "%d external apps in %s", count, apps_dir);

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

static esp_err_t launch_exclusive(const extapp_t *app, void (*progress)(int percent, void *ctx), void *ctx)
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

/* A launch cannot read an application while its files are being published. */
esp_err_t extapp_launch(const extapp_t *app, void (*progress)(int, void *), void *ctx)
{
    if (!storage_files_acquire()) return fail("Memoria file occupata", ESP_ERR_INVALID_STATE);
    esp_err_t result = launch_exclusive(app, progress, ctx);
    storage_files_release();
    return result;
}
