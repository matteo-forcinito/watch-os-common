/*
 * Dashboard for an app built into the OS (CONFIG_APPKIT_HOSTED, see appkit.h).
 *
 * Same API as appkit_web.c, but there is no second server: the app's page and
 * routes are served by the OS web server under "/<app>/" while the app is open
 * and its dashboard page has been shown once (appkit_web_activate)
 * ("/pieno/", "/pieno/api/state"...). kit.js prefixes the page's API calls with
 * that folder. Pairing is the OS one: one code pairs the browser with the whole
 * watch (system dashboard and every internal app).
 */
#include "appkit.h"
#include "appkit_private.h"

#include "core/sys.h"
#include "hardware/board.h"
#include "hardware/pmu.h"
#include "services/web_server.h"
#include "services/wifi.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "appkit_hosted";

#define FOLDER_MAX 20

static const appkit_web_config_t *s_config;
static SemaphoreHandle_t s_lock;          /* start/stop vs the start task */
static atomic_int s_state = APPKIT_WEB_OFF;
static atomic_uint s_hits;
static atomic_int s_generation;
static bool s_server_held;                /* this app holds one web_server_start() */
static char s_folder[FOLDER_MAX];         /* "/pieno" */

/* ----------------------------------------------------------- alarms */

/* The OS rings its own alarms: nothing to do for an internal app. */
void appkit_alarms_prepare(void)
{
}

void appkit_alarms_start(void)
{
}

/* ------------------------------------------------------------ access */

bool appkit_authorized(httpd_req_t *request)
{
    bool ok = web_server_authorized(request);
    if (ok)
    {
        atomic_fetch_add(&s_hits, 1);
    }
    return ok;
}

const char *appkit_web_code(void)
{
    return web_server_code();
}

void appkit_web_unpair(void)
{
    /* Pairing belongs to the whole watch (Web page app). */
}

unsigned appkit_web_hits(void)
{
    return atomic_load(&s_hits);
}

appkit_web_state_t appkit_web_state(void)
{
    return (appkit_web_state_t)atomic_load(&s_state);
}

void appkit_web_url(char *out, size_t size)
{
    if (atomic_load(&s_state) != APPKIT_WEB_ON)
    {
        snprintf(out, size, "%s", "");
        return;
    }
    char base[40];
    web_server_url(base, sizeof(base));   /* "http://192.168.1.20/" */
    snprintf(out, size, "%s%s/", base, s_folder + 1);
}

/* ------------------------------------------------------------ routes */

static esp_err_t page(httpd_req_t *request)
{
    const appkit_web_config_t *config = s_config;
    if (config == NULL)
    {
        return appkit_error(request, "404 Not Found", "App chiusa");
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    /* EMBED_TXTFILES adds a NUL at the end. */
    return httpd_resp_send(request, config->page_start, config->page_end - config->page_start - 1);
}

static esp_err_t kit_info(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    const appkit_web_config_t *config = s_config;
    const board_info_t *board = board_info();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "app", config != NULL ? config->name : "");
    cJSON_AddStringToObject(out, "board", board->product);
    cJSON_AddNumberToObject(out, "width", board->width);
    cJSON_AddNumberToObject(out, "height", board->height);
    cJSON_AddBoolToObject(out, "round", board->round);
    cJSON_AddNumberToObject(out, "battery", pmu_present() ? pmu_battery_percent() : -1);
    cJSON_AddBoolToObject(out, "charging", pmu_present() && pmu_is_charging());
    cJSON_AddNumberToObject(out, "rssi", wifi_rssi());
    cJSON_AddNumberToObject(out, "uptime", (double)(esp_timer_get_time() / 1000000));
    return appkit_json(request, out);
}

/* Every GET/POST under "/<app>/" lands here (registered once, never removed).
 * The lock is held while the app's handler runs, so appkit_web_stop() (app
 * closing) waits for a request in flight before the app frees its data. */
static esp_err_t dispatch(httpd_req_t *request)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const appkit_web_config_t *config = s_config;
    size_t folder = strlen(s_folder);
    if (config == NULL || atomic_load(&s_state) != APPKIT_WEB_ON || strncmp(request->uri, s_folder, folder) != 0 ||
        request->uri[folder] != '/')
    {
        xSemaphoreGive(s_lock);
        return appkit_error(request, "404 Not Found", "Apri l'app sull'orologio");
    }
    const char *path = request->uri + folder;   /* "/api/state?x=1" */
    size_t length = strcspn(path, "?");
    esp_err_t result = ESP_FAIL;
    bool found = false;
    if (request->method == HTTP_GET && length == 1)
    {
        result = page(request);
        found = true;
    }
    else if (request->method == HTTP_GET && length == 8 && strncmp(path, "/api/kit", 8) == 0)
    {
        result = kit_info(request);
        found = true;
    }
    for (size_t i = 0; !found && i < config->route_count; i++)
    {
        const httpd_uri_t *route = &config->routes[i];
        if ((int)route->method == request->method && strlen(route->uri) == length && strncmp(route->uri, path, length) == 0)
        {
            request->user_ctx = route->user_ctx;
            result = route->handler(request);
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found ? result : appkit_error(request, "404 Not Found", "Non trovato");
}

/* -------------------------------------------------------- start/stop */

static void start_task(void *arg)
{
    int generation = (int)(intptr_t)arg;
    bool ok = web_server_start() == ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool current = generation == atomic_load(&s_generation) && s_config != NULL;
    if (ok && current)
    {
        char wildcard[FOLDER_MAX + 2];
        snprintf(wildcard, sizeof(wildcard), "%s/*", s_folder);
        s_server_held = true;
        if (web_server_add_folder(wildcard, dispatch) != ESP_OK)
        {
            ESP_LOGE(TAG, "No room for %s", wildcard);
        }
        atomic_store(&s_state, APPKIT_WEB_ON);
        ESP_LOGI(TAG, "%s dashboard on %s/", s_config->name, s_folder);
    }
    else if (current)
    {
        atomic_store(&s_state, APPKIT_WEB_FAILED);
    }
    xSemaphoreGive(s_lock);
    if (ok && !current)
    {
        web_server_stop();   /* the app closed while Wi-Fi was joining */
    }
    vTaskDelete(NULL);
}

esp_err_t appkit_web_start(const appkit_web_config_t *config)
{
    appkit_json_hooks();
    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_config == config)
    {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_config != NULL)
    {
        xSemaphoreGive(s_lock);
        appkit_web_stop();   /* another internal app's dashboard: one at a time */
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_config = config;
    size_t i = 0;
    s_folder[i++] = '/';
    for (size_t c = 0; config->name[c] != '\0' && i < sizeof(s_folder) - 1; c++)
    {
        unsigned char ch = (unsigned char)config->name[c];
        if (isalnum(ch))
        {
            s_folder[i++] = (char)tolower(ch);
        }
    }
    s_folder[i] = '\0';
    atomic_store(&s_state, APPKIT_WEB_OFF);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void appkit_web_activate(void)
{
    if (s_lock == NULL)
    {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    appkit_web_state_t state = (appkit_web_state_t)atomic_load(&s_state);
    bool start = s_config != NULL && (state == APPKIT_WEB_OFF || state == APPKIT_WEB_FAILED);
    int generation = atomic_load(&s_generation);
    if (start)
    {
        atomic_store(&s_state, APPKIT_WEB_JOINING);
    }
    xSemaphoreGive(s_lock);
    if (start && !sys_task_create(start_task, "appkit_web", 4096, (void *)(intptr_t)generation, 4, SYS_CORE_ANY))
    {
        atomic_store(&s_state, APPKIT_WEB_FAILED);
    }
}

static void stop_task(void *arg)
{
    (void)arg;
    web_server_stop();
    vTaskDelete(NULL);
}

/* Called from the app's close (LVGL task): never blocks on the network. */
void appkit_web_stop(void)
{
    if (s_lock == NULL)
    {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    atomic_fetch_add(&s_generation, 1);
    bool held = s_server_held;
    s_server_held = false;
    s_config = NULL;
    atomic_store(&s_state, APPKIT_WEB_OFF);
    xSemaphoreGive(s_lock);
    if (held)
    {
        sys_task_create(stop_task, "appkit_stop", 4096, NULL, 3, SYS_CORE_ANY);
    }
}
