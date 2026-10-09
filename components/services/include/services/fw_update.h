#ifndef SERVICES_FW_UPDATE_H
#define SERVICES_FW_UPDATE_H

/*
 * Firmware update of the base system, without a USB cable.
 *
 *   from the PC    web page -> "Aggiornamento firmware" -> build/<project>.bin
 *                  (the web server streams it in with fw_update_begin/write/end)
 *   online         a JSON catalog (settings.update_url, or the firmware's
 *                  CONFIG_WATCH_UPDATE_MANIFEST_URL if that is empty), written by
 *                  the release CI (.github/workflows/release.yml):
 *                      {"version": "1.2.0", "url": ".../watcher_os.bin",      <- stable, for old firmwares
 *                       "channels": {"stable": {...}, "dev": {...}},
 *                       "releases": [{"version", "channel", "date", "notes", "url", "size"}, ...]}
 *                  fw_update_check_online() looks at the chosen channel (settings);
 *                  fw_update_fetch_releases() lists everything, to install ANY
 *                  version (also an older one) with fw_update_install_release().
 *   SD card        /sdcard/firmware/<project>-<version>.bin: before an update the
 *                  running firmware is saved there and the new one is kept too, so
 *                  any of them can be reinstalled without network
 *                  (fw_update_list_sd / fw_update_install_file). No card: cloud only.
 *
 * The new firmware goes into the other app slot (the one external apps use),
 * then the watch restarts into it. Safety:
 *   - the file is checked BEFORE the slot is touched: right chip and project
 *     name as the running one (an external app, the other watch's firmware or
 *     a merged .bin is refused)
 *   - esp_ota_end() verifies the SHA-256 of the whole image
 *   - rollback: the new firmware must reach the end of its boot and call
 *     fw_update_confirm_boot(); if it crashes before, the bootloader goes
 *     back to the previous firmware by itself
 *
 * Progress is published as STATE_UPDATE (phase) and STATE_UPDATE_PERCENT,
 * changed only when the whole percent changes: at most ~100 screen updates,
 * nothing that slows the transfer.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    FW_UPDATE_IDLE,
    FW_UPDATE_CHECKING,     /* online: reading the manifest */
    FW_UPDATE_UP_TO_DATE,
    FW_UPDATE_AVAILABLE,    /* online: a newer version exists (fw_update_new_version) */
    FW_UPDATE_WRITING,      /* receiving / writing, see STATE_UPDATE_PERCENT */
    FW_UPDATE_RESTARTING,   /* done, restarting into the new firmware */
    FW_UPDATE_FAILED,       /* fw_update_message() says why */
} fw_update_phase_t;

/* Manifest address in use: the user's, else the built-in default ("" if none). */
const char *fw_update_manifest_url(void);

/* Running firmware version (PROJECT_VER in the root CMakeLists.txt). */
const char *fw_update_running_version(void);

fw_update_phase_t fw_update_phase(void);
int fw_update_percent(void);

/* Error or status text for the last operation ("" if none). */
const char *fw_update_message(void);

/* Version found online (after FW_UPDATE_AVAILABLE). */
const char *fw_update_new_version(void);

/*
 * Streaming install, from any task, one at a time. `total_size` = bytes that
 * will follow. Each call returns an error to stop on; after an error the
 * update is already aborted.
 */
esp_err_t fw_update_begin(size_t total_size);
esp_err_t fw_update_write(const void *data, size_t length);
esp_err_t fw_update_end(void);              /* verify, select, restart in ~2 s */
void fw_update_abort(const char *reason);

/* Check the catalog for the chosen channel (and install if `install`), in a background task. */
void fw_update_check_online(bool install);

/* ---- every release: browse and install any version */

#define FW_RELEASES_MAX  30
#define FW_LOCAL_MAX     12
#define FW_SD_DIR        "/sdcard/firmware"

typedef enum
{
    FW_CHANNEL_STABLE = 0,
    FW_CHANNEL_DEV = 1,
} fw_channel_t;

typedef struct
{
    char version[32];
    char channel[8];     /* "stable" or "dev" */
    char date[12];       /* "2026-10-10" */
    char notes[80];
    char url[192];
    uint32_t size;
} fw_release_t;

typedef struct
{
    char path[112];   /* FW_SD_DIR "/" + file name */
    char version[32];
} fw_local_t;

/*
 * The release catalog, newest first (both channels). BLOCKING: Wi-Fi and
 * HTTPS, a few seconds - call it from a task, never from the lvgl task.
 */
esp_err_t fw_update_fetch_releases(fw_release_t *out, int max, int *count);

/* Install this release, whatever its version (downgrade too), in a background task. */
void fw_update_install_release(const fw_release_t *release);

/* Firmware files for THIS device on the SD card, newest first. Reads the card: not in the lvgl task. */
int fw_update_list_sd(fw_local_t *out, int max);

/* Install a firmware file from the SD card, in a background task. */
void fw_update_install_file(const char *path);

/* Call when the boot finished well: the running firmware is kept for good. */
void fw_update_confirm_boot(void);

#endif
