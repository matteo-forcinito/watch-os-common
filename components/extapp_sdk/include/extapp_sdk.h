#ifndef EXTAPP_SDK_H
#define EXTAPP_SDK_H

/*
 * For external apps (firmwares started by the launcher from the SD card).
 *
 *     void app_main(void)
 *     {
 *         extapp_sdk_init();      // first thing: arm the way back
 *         ...
 *         extapp_return_to_launcher();   // e.g. on BOOT
 *     }
 *
 * extapp_sdk_init() makes the launcher the boot app again right away. So the
 * app runs only once: whatever happens next - return, crash, watchdog,
 * battery pulled - the next start is the launcher. No way to get stuck.
 *
 * The launcher is the slot the OS recorded in NVS ("extapp"/"launcher"),
 * so apps can live in any slot of an app bay (ota_2, ota_3...). Without that
 * key it is "the other" slot (the rule the old Arduino apps used).
 *
 * EXTAPP_SDK_VERSION 2 = bay-aware: publish with "sdk": 2 in the market catalog.
 */

#define EXTAPP_SDK_VERSION 2

#include "esp_err.h"

esp_err_t extapp_sdk_init(void);

/* Restart into the launcher. Does not return. */
void extapp_return_to_launcher(void);

/*
 * Restart this app once more (e.g. to free the USB port). The next restart
 * after that goes to the launcher again. Does not return.
 */
void extapp_restart_self(void);

#endif
