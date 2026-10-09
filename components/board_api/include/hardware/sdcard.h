#ifndef HARDWARE_SDCARD_H
#define HARDWARE_SDCARD_H

/*
 * microSD card, mounted at /sdcard (FAT32).
 *
 *   /sdcard/apps/<name>/<name>.bin   external apps (+ icon.png)
 *   /sdcard/config/radios.txt        web radio stations
 *   /sdcard/config/wifi.txt          known Wi-Fi networks (ssid = password)
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sd_protocol_types.h"

#define SDCARD_MOUNT "/sdcard"

/* Mount (idempotent). */
esp_err_t sdcard_mount(void);
void sdcard_unmount(void);
bool sdcard_is_mounted(void);

/*
 * For USB mass storage: unmount the files and hand the raw card to the USB
 * stack (the PC owns the card from then on; leave with a restart).
 */
esp_err_t sdcard_open_raw(sdmmc_card_t **card);

/* Card size and free space in MB (0 if not mounted). */
void sdcard_space(uint32_t *total_mb, uint32_t *free_mb);

typedef enum
{
    SDCARD_ABSENT,       /* no card (or it does not answer) */
    SDCARD_MOUNTED,      /* files available under SDCARD_MOUNT */
    SDCARD_UNREADABLE,   /* the card answers but has no FAT file system: format it */
    SDCARD_BUSY,         /* lent to the PC (USB drive) */
} sdcard_state_t;

sdcard_state_t sdcard_state(void);

typedef struct
{
    char name[8];          /* from the card's ID register, e.g. "SD32G" */
    uint32_t size_mb;      /* raw card size */
    uint32_t freq_khz;     /* bus clock in use */
    bool high_capacity;    /* SDHC/SDXC */
} sdcard_info_t;

/* Details of the mounted card. ESP_ERR_INVALID_STATE if not mounted. */
esp_err_t sdcard_info(sdcard_info_t *out);

/*
 * Erase everything: new FAT32 file system, then mounted. Works on an
 * unreadable card too. Slow (seconds to a minute): never from the lvgl task.
 */
esp_err_t sdcard_format(void);

#endif
