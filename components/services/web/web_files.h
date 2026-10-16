#ifndef WEB_FILES_H
#define WEB_FILES_H

/*
 * SD card file manager for the setup web page.
 *
 *   GET  /api/fs/list?path=/apps              folder listing (JSON)
 *   GET  /api/fs/get?path=/config/wifi.txt    download / read a file (&dl=1: as attachment)
 *   POST /api/fs/put?path=/config/wifi.txt    upload / save a file (raw body)
 *   POST /api/fs/mkdir?path=/apps/New         new folder
 *   POST /api/fs/rename?from=/a.txt&to=/b.txt rename or move
 *   POST /api/fs/delete?path=/old             delete a file or a whole folder
 *
 * Paths are relative to the card ("/" = /sdcard). ".." and other escapes are
 * refused. Uploads go to "<name>.part" and replace the file only when
 * complete, so an interrupted upload never leaves half a file.
 */

#include "esp_http_server.h"

/* Register the handlers above (6 URI handlers). */
void web_files_register(httpd_handle_t server);

/* Register a route; /api/ routes (except /api/pair) need the paired browser's token. */
void web_route(httpd_handle_t server, const httpd_uri_t *route);
/* Device, apps, market and system update API (web_system.c). */
void web_system_register(httpd_handle_t server);
/* SD maintenance is active: only its status API remains available. */
bool web_storage_maintenance(void);

#endif
