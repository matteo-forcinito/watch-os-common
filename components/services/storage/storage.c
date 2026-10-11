#include "services/storage.h"

#include "hardware/sdcard.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "storage";

#define PARTITION_LABEL "storage"

static wl_handle_t s_wl = WL_INVALID_HANDLE;

/* Only a completely erased partition may be formatted automatically. */
static bool partition_empty(const esp_partition_t *part)
{
    uint8_t *buffer = malloc(4096);
    if (buffer == NULL)
    {
        return false;
    }
    bool empty = true;
    for (size_t offset = 0; offset < part->size && empty; offset += 4096)
    {
        size_t length = part->size - offset < 4096 ? part->size - offset : 4096;
        if (esp_partition_read(part, offset, buffer, length) != ESP_OK)
        {
            empty = false;
            break;
        }
        for (size_t i = 0; i < length; i++)
        {
            if (buffer[i] != 0xff)
            {
                empty = false;
                break;
            }
        }
    }
    free(buffer);
    return empty;
}

esp_err_t storage_init(void)
{
    if (s_wl != WL_INVALID_HANDLE)
    {
        return ESP_OK;
    }

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
                                                            PARTITION_LABEL);

    if (part == NULL)
    {
        return ESP_ERR_NOT_FOUND;   /* this board keeps its files on the card only */
    }

    esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = false,   /* first start: an empty file system */
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };

    /* Wear levelling writes its metadata even if the FAT mount then fails. */
    bool can_format = partition_empty(part);
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_FLASH_MOUNT, PARTITION_LABEL, &config, &s_wl);

    if (err != ESP_OK && can_format)
    {
        config.format_if_mount_failed = true;
        err = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_FLASH_MOUNT, PARTITION_LABEL, &config, &s_wl);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Flash file system: %s", esp_err_to_name(err));
        s_wl = WL_INVALID_HANDLE;
        return err;
    }

    size_t total_kb = 0;
    size_t free_kb = 0;
    storage_flash_space(&total_kb, &free_kb);
    ESP_LOGI(TAG, "Flash file system on %s: %u KB, %u KB free", STORAGE_FLASH_MOUNT, (unsigned)total_kb,
             (unsigned)free_kb);

    return ESP_OK;
}

bool storage_flash_mounted(void)
{
    return s_wl != WL_INVALID_HANDLE;
}

const char *storage_dir(void)
{
    if (sdcard_is_mounted())
    {
        return SDCARD_MOUNT;
    }

    return storage_flash_mounted() ? STORAGE_FLASH_MOUNT : NULL;
}

bool storage_config_path(const char *name, char *out, size_t size)
{
    const char *dir = storage_dir();

    if (dir == NULL)
    {
        return false;
    }

    char folder[32];
    snprintf(folder, sizeof(folder), "%s/config", dir);

    if (mkdir(folder, 0775) != 0 && errno != EEXIST)
    {
        return false;
    }

    int written = snprintf(out, size, "%s/%s", folder, name);

    return written > 0 && (size_t)written < size;
}

void storage_flash_space(size_t *total_kb, size_t *free_kb)
{
    uint64_t total = 0;
    uint64_t free_bytes = 0;

    if (storage_flash_mounted() && esp_vfs_fat_info(STORAGE_FLASH_MOUNT, &total, &free_bytes) == ESP_OK)
    {
        *total_kb = (size_t)(total / 1024);
        *free_kb = (size_t)(free_bytes / 1024);
        return;
    }

    *total_kb = 0;
    *free_kb = 0;
}
