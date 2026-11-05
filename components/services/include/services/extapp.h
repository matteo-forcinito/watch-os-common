#ifndef SERVICES_EXTAPP_H
#define SERVICES_EXTAPP_H

/*
 * External apps: complete firmwares on the SD card, run from the second
 * app slot of the flash.
 *
 *   /sdcard/apps/WebRadio/WebRadio.bin     the firmware (Arduino or ESP-IDF)
 *   /sdcard/apps/WebRadio/icon.png         optional icon (or icon.bin, LVGL 9)
 *   /sdcard/apps/WebRadio/manifest.json    optional {"name": "...", "bin": "..."}
 *
 *   launcher (ota_0) ── copy .bin to ota_1, boot ota_1, restart ──► app
 *   app ── extapp_return_to_launcher() / BOOT ──► boot ota_0, restart ──► launcher
 *
 * Launching the same app again skips the copy (size and date of the .bin
 * are remembered), so it starts in about a second.
 *
 * Same layout as the Arduino launcher, so the old apps keep working.
 *
 * Without a card (AMOLED 1.75, or a card removed) the market writes an app
 * straight into the app slot and notes its name, version and icon here: the
 * scan then lists that one app ("in_slot"), and launching it only boots the
 * slot. Nothing is kept on a card.
 *
 * App bay: a partition table with app slots beyond ota_1 (ota_2, ota_3...)
 * keeps one market app per slot, all installed together, and the OS stays in
 * ota_0/ota_1. Launching a bay app only boots its slot. Each slot has its own
 * note in NVS. Bay apps must find the launcher through the NVS key that
 * boot_into() writes (extapp_sdk >= 2): the market only installs those.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

#define EXTAPP_MAX       24
#define EXTAPP_PATH_MAX  200

typedef struct
{
    char id[32];                  /* folder name */
    char name[32];
    char bin[EXTAPP_PATH_MAX];    /* /sdcard/apps/x/x.bin */
    char icon[EXTAPP_PATH_MAX];   /* "S:/apps/x/icon.png" for LVGL, "" if none */
    size_t size;
    char version[32];
    bool in_slot;                 /* lives only in the app slot (no file to copy) */
    uint8_t sdk;                  /* extapp_sdk generation of a card app (manifest "sdk"; 1 if not stated) */
} extapp_t;

int extapp_scan(extapp_t *out, int max);

/* Retarget metadata after manual upload. Caller owns the storage file lease. */
esp_err_t extapp_upload_manifest(const char *folder, const char *binary_name);

/* True if this app is already in the app slot (no copy needed). */
bool extapp_is_cached(const extapp_t *app);

/*
 * Copy (if needed) and start the app. Blocking: run it in a task. Restarts
 * the chip on success; returns only on error. progress(0..100) is optional.
 */
esp_err_t extapp_launch(const extapp_t *app, void (*progress)(int percent, void *ctx), void *ctx);

/* The app slot is being overwritten (e.g. firmware update): no app is cached any more. */
void extapp_forget_cache(void);

/* App slots beyond ota_1 (0 = classic layout: one app slot shared with OS updates). */
int extapp_bay_size(void);
/* Bay slot `index` (0 = ota_2), NULL past the end. */
const esp_partition_t *extapp_bay_slot(int index);
/* Where an OS update goes: the OS's other slot, never a bay slot. */
const esp_partition_t *extapp_os_update_slot(void);
/* Where the market writes app `id` of `size` bytes: its current slot, else a free
 * one that fits. NULL if none (bay full, or too large). */
const esp_partition_t *extapp_slot_for(const char *id, size_t size);
/* The slot an in_slot app lives in ("slot:<label>"), or NULL. */
const esp_partition_t *extapp_slot_of(const extapp_t *app);
/* The app held by `slot`, if it holds one (not an OS image). */
bool extapp_slot_read(const esp_partition_t *slot, extapp_t *out);
/* Every app held by app slots (one in the classic layout, one per bay slot). */
int extapp_slot_apps(extapp_t *out, int max);
/* An app was written straight into `slot`: remember what it is.
 * icon: LVGL path ("F:/apps/x/icon.png") or "". */
esp_err_t extapp_slot_note(const esp_partition_t *slot, const char *id, const char *name, const char *version,
                           const char *icon);
/* Remove app `id` from every bay slot but `keep` (an update moved it to a larger slot). */
void extapp_slot_drop_others(const esp_partition_t *keep, const char *id);
/* The app in `slot` was removed (or replaced by something else). */
void extapp_slot_forget(const esp_partition_t *slot);
/* Record the running OS slot as the launcher apps return to (boot and launch). */
void extapp_note_launcher(void);
/* The first app in an app slot (classic layout: the only one). */
bool extapp_slot_app(extapp_t *out);
/* Install a card app into an app bay slot (offline, from the card), without
 * starting it. Blocking: run it in a task. progress(0..100) is optional. */
esp_err_t extapp_install(const extapp_t *app, const esp_partition_t *slot, void (*progress)(int percent, void *ctx),
                         void *ctx);
/* Error text of the last failed launch. */
const char *extapp_last_error(void);

#endif
