#ifndef APPKIT_PRIVATE_H
#define APPKIT_PRIVATE_H

#include <stddef.h>

/* Shared by appkit's source files only. */

/* cJSON allocations in PSRAM (internal RAM is scarce). */
void appkit_json_hooks(void);

/* The dashboard page is on screen: an internal app's dashboard starts now
 * (Wi-Fi and the server only when someone wants them). No-op for external apps,
 * whose server starts with appkit_web_start(). */
void appkit_web_activate(void);

/* The dashboard's name on the network: "tamiro.local/pieno/" (internal app)
 * or "pieno.local" (external app). */
void appkit_web_host(char *out, size_t size);

#endif
