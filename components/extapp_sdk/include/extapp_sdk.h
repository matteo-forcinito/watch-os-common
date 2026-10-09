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
 * Works with any app slot layout where the launcher is "the other" slot
 * (the same rule the old Arduino apps used).
 */

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
