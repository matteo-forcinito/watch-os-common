/* pt_watch_os.h - pocket-tank hosted as a Watch OS external app.
 *
 * The app runs from ota_1 next to the launcher in ota_0, on the launcher's
 * partition table (no "model" partition, no second slot of its own). What
 * changes against the stand-alone firmware:
 *   - every reset goes back to the launcher, except a deep sleep, which wakes
 *     into the tank (pt_watch_os_before_deep_sleep);
 *   - BOOT short press leaves to the launcher;
 *   - NVS is shared with the OS: never erased, Wi-Fi keys in "ptwifi";
 *   - Wi-Fi updates and provisioning are off (they would write ota_0);
 *   - the 7.56 MB model sits in two unpartitioned flash runs and is mapped
 *     as one contiguous block (pt_watch_os_model_map).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Flash runs that hold the model: the tail of ota_1 past the app's room,
 * then everything after the coredump partition up to the end of the chip.
 * The app image must stay below PT_MODEL_RUN_A. */
#define PT_APP_SLOT          0x620000u
#define PT_MODEL_RUN_A       0x880000u
#define PT_MODEL_RUN_A_END   0xC20000u
#define PT_MODEL_RUN_B       0xC60000u
#define PT_MODEL_RUN_B_END   0x1000000u
/* The last 4 KB sector of run B: magic, length and SHA-256 of the model,
 * written by tools/install_usb.py after the body. */
#define PT_MODEL_TRAILER     0xFFF000u
#define PT_MODEL_MAGIC       0x4D545450u   /* "PTTM" */

void pt_watch_os_boot(void);
bool pt_watch_os_model_map(const void **map, size_t *len);
void pt_watch_os_before_deep_sleep(void);
void pt_watch_os_exit(void);
