#ifndef APPKIT_H
#define APPKIT_H

/*
 * appkit: what every external app needs besides its own feature.
 *
 *   Alarms     The OS alarms keep ringing while the app runs (the app replaces
 *              the OS until BOOT): appkit_alarms_prepare() before the UI,
 *              appkit_alarms_start() after it.
 *
 *   Dashboard  A web page served by the watch, opened from a phone or a PC on
 *              the same Wi-Fi. The watch shows a QR code (address + pairing
 *              code); the browser trades the code for a token it keeps, so it
 *              stays paired across restarts. Every API call carries the token
 *              in X-Key and must come from the page itself (same origin).
 *
 *              The app embeds its page (dashboard.html) and lists its routes;
 *              appkit serves "/", "/kit.js", "/kit.css", "/api/pair" and
 *              "/api/kit" (app name, board, battery). kit.js gives the page
 *              the pairing screen, kit.api() and toasts.
 *
 * Starting the dashboard joins Wi-Fi in a background task (never in the
 * caller), so it can be called from the UI.
 */

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "lvgl.h"

/* ------------------------------------------------------------------ alarms */

void appkit_alarms_prepare(void);
void appkit_alarms_start(void);

/* --------------------------------------------------------------- dashboard */

typedef struct
{
    const char *name;            /* "Pieno": page title, mDNS host "pieno.local" */
    const char *page_start;      /* embedded dashboard.html (EMBED_TXTFILES) */
    const char *page_end;
    const httpd_uri_t *routes;   /* the app's own API (check appkit_authorized()) */
    size_t route_count;
    bool hotspot;                /* no known network: open the device's own hotspot (in the car) */
} appkit_web_config_t;

typedef enum
{
    APPKIT_WEB_OFF,
    APPKIT_WEB_JOINING,          /* waiting for Wi-Fi */
    APPKIT_WEB_ON,
    APPKIT_WEB_FAILED,           /* no known network reachable: retried every 30 s */
    APPKIT_WEB_NO_MEMORY,        /* internal app: not enough internal RAM to start Wi-Fi */
} appkit_web_state_t;

/* Start serving (non-blocking). The config must stay valid while running. */
esp_err_t appkit_web_start(const appkit_web_config_t *config);
/* Activate the registered dashboard without opening its app UI (non-blocking). */
void appkit_web_activate(void);
void appkit_web_stop(void);
/* Stop only when the caller still owns the hosted dashboard. LVGL task only. */
void appkit_web_stop_if(const appkit_web_config_t *config);
/* Ownership changes only in the LVGL task. This check never waits for HTTP. */
bool appkit_web_owned_by(const appkit_web_config_t *config);
appkit_web_state_t appkit_web_state(void);

/* "http://192.168.1.20/" once on, else "". */
void appkit_web_url(char *out, size_t size);
/* The hotspot's network name while the dashboard runs on it, else "". */
void appkit_web_hotspot(char *out, size_t size);
/* Six digits, new at every start. */
const char *appkit_web_code(void);
/* Forget the paired browsers (a new token). */
void appkit_web_unpair(void);
/* Count of API calls served (changes when someone uses the dashboard). */
unsigned appkit_web_hits(void);

/* Route helpers. */
bool appkit_authorized(httpd_req_t *request);
/* Request body, NUL-terminated, in PSRAM (free it); NULL if empty or over limit. */
char *appkit_body(httpd_req_t *request, size_t limit);
/* Sends and deletes json (NULL -> 503). */
esp_err_t appkit_json(httpd_req_t *request, cJSON *json);
esp_err_t appkit_error(httpd_req_t *request, const char *status, const char *text);
/* "401 Unauthorized" for routes that need pairing. */
esp_err_t appkit_unauthorized(httpd_req_t *request);
/* Query parameter as text ("" if missing). */
bool appkit_query(httpd_req_t *request, const char *key, char *out, size_t size);

/* Internal apps (CONFIG_APPKIT_HOSTED), at boot: the dashboard folder
 * "/<name>/" exists from now on. Opening its page with the app closed opens
 * the app on the watch (and its dashboard) - nothing runs until then. */
void appkit_hosted_register(const char *name, const char *app_id);

/* ---------------------------------------------------------------------- UI */

/* The dashboard page for the app's menu: QR code, address, pairing code, state.
 * Fills `parent` (a page from ui_page()); updates itself every second. */
void appkit_dashboard_page(lv_obj_t *parent, lv_color_t accent);

#endif
