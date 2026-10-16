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
#include "esp_http_server.h"

/* Connect (or start the access point) and serve. Blocking up to ~15 s.
 * Shared: the Web app and the internal apps' dashboards each start it, and it
 * stops when the last of them calls web_server_stop(). */
esp_err_t web_server_start(void);
/* Same, but never the access point: internal apps' dashboards (the access
 * point costs ~30 KB of internal RAM and takes the watch off the home Wi-Fi). */
esp_err_t web_server_start_wifi_only(void);
/* Recovery worker only: fallback hotspot and bounded network retry. */
void web_server_recovery_poll(void);
void web_server_stop(void);
bool web_server_running(void);

/* Why the last start failed (for the screen). */
const char *web_server_last_error(void);

/* "http://192.168.1.42/" and, in access point mode, the network name. */
void web_server_url(char *out, size_t size);

/* Six digits that pair a new browser (shown on the watch and in its QR as #code=...). */
const char *web_server_code(void);
/* Nonblocking snapshot for authorized Family requests; never returns an API token. */
bool web_server_ui_pending(void);
bool web_server_pairing_snapshot(char code[7], bool *recovery);
bool web_server_ap_name(char *out, size_t size);

/* For internal apps' dashboards (appkit hosted mode): `handler` gets every GET
 * and POST under `folder` ("/pieno/" + "*"), now and after each server restart.
 * Routes are never removed (the server is not thread safe for that): the
 * handler answers "app closed" itself. */
esp_err_t web_server_add_folder(const char *folder, const char *app_id, esp_err_t (*handler)(httpd_req_t *req));
/* "/pieno/" for the app with a dashboard folder, NULL otherwise. */
const char *web_server_app_folder(const char *app_id, char *out, size_t size);
/* The request comes from the paired browser (token, same origin). */
bool web_server_authorized(httpd_req_t *req);

#endif
