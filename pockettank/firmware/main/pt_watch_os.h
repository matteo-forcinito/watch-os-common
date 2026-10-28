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
#include <stdint.h>

/* Flash runs that hold the model, outside every partition. Default (16 MB
 * boards whose table ends at the coredump, e.g. AMOLED 1.8): the tail of ota_1
 * past the app's room (the app image must stay below run A), then everything
 * after the coredump. A board wrapper may override them (AMOLED 1.75C: one run
 * above 16 MB, run B empty). Each run is 64 KB aligned. */
#ifndef PT_MODEL_RUN_A
#define PT_MODEL_RUN_A       0x880000u
#define PT_MODEL_RUN_A_END   0xC20000u
#define PT_MODEL_RUN_B       0xC60000u
#define PT_MODEL_RUN_B_END   0x1000000u
#endif
/* The last 4 KB sector of the last run: magic, length and SHA-256 of the
 * model, written after the body (tools/install_usb.py or the downloader). */
#define PT_MODEL_TRAILER     ((PT_MODEL_RUN_B_END > PT_MODEL_RUN_B ? PT_MODEL_RUN_B_END : PT_MODEL_RUN_A_END) - 0x1000u)
#define PT_MODEL_MAGIC       0x4D545450u   /* "PTTM" */
/* The model the app downloads when the runs hold none (upstream 3df1049, schema v4). */
#define PT_MODEL_URL         "https://github.com/matteo-forcinito/watch-firmware-releases/releases/download/pockettank-model-v4/model_q4.bin"
#define PT_MODEL_LEN         7557640u
#define PT_MODEL_SHA256      "50bed3e16a6b11f3496dde21d8784da9067f40ec9f0a77cf97f60a95acb80f6b"

void pt_watch_os_boot(void);
bool pt_watch_os_model_map(const void **map, size_t *len);
/* Downloads the model over the Watch OS Wi-Fi into the runs, with a progress
 * page on fb; a touch skips it. True when a verified model is in place. */
bool pt_watch_os_model_install(uint16_t *fb);
void pt_watch_os_before_deep_sleep(void);
void pt_watch_os_exit(void);
