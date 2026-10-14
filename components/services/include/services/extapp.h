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
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

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

/* An app was written straight into the app slot: remember what it is.
 * icon: LVGL path ("F:/apps/x/icon.png") or "". */
esp_err_t extapp_slot_note(const char *id, const char *name, const char *version, const char *icon);
/* The slot app was removed (or replaced by something else). */
void extapp_slot_forget(void);
/* The app currently in the app slot, if the slot holds one (not an OS image). */
bool extapp_slot_app(extapp_t *out);

/* Error text of the last failed launch. */
const char *extapp_last_error(void);

#endif
