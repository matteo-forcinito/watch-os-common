#ifndef SERVICES_STORAGE_H
#define SERVICES_STORAGE_H

/*
 * Where files are kept: the microSD card if there is one, else a small FAT
 * file system in the chip's own flash.
 *
 *   storage_dir()  "/sdcard"   card mounted
 *                  "/data"     no card, but the partition table has a FAT
 *                              partition named "storage" (boards without a
 *                              card slot: AMOLED 1.75)
 *                  NULL        neither: only the built-in defaults
 *
 * Settings that must survive (Wi-Fi networks, alarms, favourites) live in
 * NVS anyway; this is for files: the station list, configuration texts.
 * The flash partition is mounted with wear levelling: write files rarely
 * (on a change), not every second.
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define STORAGE_FLASH_MOUNT "/data"

/* Mount the flash partition if the board has one (formats it the first time). After board_init(). */
esp_err_t storage_init(void);

/* The directory for files (see above), or NULL. */
const char *storage_dir(void);

/* storage_dir() + "/config/<name>", the config folder created if needed. false if no storage. */
bool storage_config_path(const char *name, char *out, size_t size);

/* The flash file system is mounted (whether or not a card is in use). */
bool storage_flash_mounted(void);

/* Size and free space of the flash file system, in KB (0 if not mounted). */
void storage_flash_space(size_t *total_kb, size_t *free_kb);

/* Nonblocking lease for app transfers, launches and USB card handover. */
bool storage_files_acquire(void);
void storage_files_release(void);
bool storage_files_busy(void);

#endif
