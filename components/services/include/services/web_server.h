#ifndef SERVICES_WEB_SERVER_H
#define SERVICES_WEB_SERVER_H

/*
 * Setup web page, opened from a phone or PC browser:
 * Wi-Fi networks, alarms, radio stations, settings, time, app upload.
 *
 *   - with a known Wi-Fi in range: http://<ip>/ or http://watch.local/
 *   - otherwise the watch creates its own network "<product>-XXXX" (e.g. "AMOLED-Watch-XXXX")
 *     and the page is at http://192.168.4.1/
 *
 * Runs only while the "Web" app is open (Wi-Fi costs battery).
 * The page is a single HTML file (web/index.html) talking to a small JSON API.
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Connect (or start the access point) and serve. Blocking up to ~15 s. */
esp_err_t web_server_start(void);
void web_server_stop(void);
bool web_server_running(void);

/* Why the last start failed (for the screen). */
const char *web_server_last_error(void);

/* "http://192.168.1.42/" and, in access point mode, the network name. */
void web_server_url(char *out, size_t size);

/* Six digits that pair a new browser (shown on the watch and in its QR as #code=...). */
const char *web_server_code(void);
bool web_server_ap_name(char *out, size_t size);

#endif
