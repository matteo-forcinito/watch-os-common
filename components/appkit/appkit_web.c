/*
 * Dashboard server shared by the external apps (see appkit.h).
 *
 * The server task's stack is in internal RAM: handlers may write NVS or
 * files, which disables the flash cache (a PSRAM stack would then assert).
 */
#include "appkit.h"
#include "appkit_private.h"

#include "core/sys.h"
#include "hardware/board.h"
#include "hardware/pmu.h"
#include "services/wifi.h"
#include "extapp_sdk.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mdns.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "appkit_web";

extern const char kit_js_start[] asm("_binary_kit_js_start");
extern const char kit_js_end[] asm("_binary_kit_js_end");
extern const char kit_css_start[] asm("_binary_kit_css_start");
extern const char kit_css_end[] asm("_binary_kit_css_end");

#define NVS_NAMESPACE "appkit"
#define EXTRA_ROUTES  6

static const appkit_web_config_t *s_config;
static httpd_handle_t s_server;
static atomic_int s_state = APPKIT_WEB_OFF;
static atomic_bool s_stop;
static atomic_uint s_hits;
static bool s_wifi_held;
static bool s_mdns;
static char s_token[33];
static char s_code[7];
static char s_url[40];
static char s_key[16];          /* NVS key: the app name, lower case */
static int64_t s_rate_reset;
static int s_attempts;

/* -------------------------------------------------------------- helpers */

static bool same_origin(httpd_req_t *request)
{
    if (httpd_req_get_hdr_value_len(request, "Origin") == 0)
    {
        return true;   /* same-origin GETs and old browsers send no Origin */
    }
    char origin[100], host[80], expected[100];
    if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK)
    {
        return false;
    }
    snprintf(expected, sizeof(expected), "http://%s", host);
    return strcmp(origin, expected) == 0;
}

bool appkit_authorized(httpd_req_t *request)
{
    char token[40];
    if (!same_origin(request) || s_token[0] == '\0' ||
        httpd_req_get_hdr_value_str(request, "X-Key", token, sizeof(token)) != ESP_OK || strlen(token) != 32)
    {
        return false;
    }
    unsigned difference = 0;
    for (int i = 0; i < 32; i++)
    {
        difference |= (unsigned)(token[i] ^ s_token[i]);
    }
    if (difference == 0)
    {
        atomic_fetch_add(&s_hits, 1);
    }
    return difference == 0;
}

/* --------------------------------------------------------------- tokens */

static void new_token(void)
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    for (int i = 0; i < 16; i++)
    {
        snprintf(s_token + i * 2, 3, "%02x", bytes[i]);
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_str(nvs, s_key, s_token);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* The token survives restarts: a paired browser keeps working. */
static void load_token(void)
{
    nvs_handle_t nvs;
    size_t size = sizeof(s_token);
    bool ok = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK)
    {
        ok = nvs_get_str(nvs, s_key, s_token, &size) == ESP_OK && strlen(s_token) == 32;
        nvs_close(nvs);
    }
    if (!ok)
    {
        new_token();
    }
}

void appkit_web_unpair(void)
{
    new_token();   /* old browsers get 401 and show the pairing screen again */
}

const char *appkit_web_code(void)
{
    return s_code;
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
    snprintf(out, size, "%s", atomic_load(&s_state) == APPKIT_WEB_ON ? s_url : "");
}

/* --------------------------------------------------------------- routes */

static esp_err_t send_static(httpd_req_t *request, const char *type, const char *start, const char *end)
{
    httpd_resp_set_type(request, type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    /* EMBED_TXTFILES adds a NUL at the end. */
    return httpd_resp_send(request, start, end - start - 1);
}

static esp_err_t page(httpd_req_t *request)
{
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    return send_static(request, "text/html; charset=utf-8", s_config->page_start, s_config->page_end);
}

static esp_err_t kit_js(httpd_req_t *request)
{
    return send_static(request, "text/javascript; charset=utf-8", kit_js_start, kit_js_end);
}

static esp_err_t kit_css(httpd_req_t *request)
{
    return send_static(request, "text/css; charset=utf-8", kit_css_start, kit_css_end);
}

static esp_err_t pair(httpd_req_t *request)
{
    if (!same_origin(request))
    {
        return appkit_error(request, "403 Forbidden", "Origine non consentita");
    }
    int64_t now = esp_timer_get_time();
    if (now >= s_rate_reset)
    {
        s_attempts = 0;
        s_rate_reset = now + 60000000;
    }
    if (++s_attempts > 6)
    {
        return appkit_error(request, "429 Too Many Requests", "Troppi tentativi: riprova tra un minuto");
    }
    char *text = appkit_body(request, 128);
    cJSON *root = text != NULL ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    bool ok = cJSON_IsString(code) && strcmp(code->valuestring, s_code) == 0;
    cJSON_Delete(root);
    if (!ok)
    {
        return appkit_error(request, "401 Unauthorized", "Codice errato: guarda l'orologio");
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "token", s_token);
    return appkit_json(request, out);
}

static esp_err_t kit_info(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    const board_info_t *board = board_info();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "app", s_config->name);
    cJSON_AddStringToObject(out, "board", board->product);
    cJSON_AddNumberToObject(out, "width", board->width);
    cJSON_AddNumberToObject(out, "height", board->height);
    cJSON_AddBoolToObject(out, "round", board->round);
    cJSON_AddNumberToObject(out, "battery", pmu_present() ? pmu_battery_percent() : -1);
    cJSON_AddBoolToObject(out, "charging", pmu_present() && pmu_is_charging());
    cJSON_AddNumberToObject(out, "rssi", wifi_rssi());
    cJSON_AddNumberToObject(out, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(out, "exit", true);   /* POST /api/kit/exit returns to Watch OS */
    return appkit_json(request, out);
}

/* Orbit asks to go back to Watch OS: reply first, restart a moment later. */
static void exit_later(void *arg)
{
    (void)arg;
    extapp_return_to_launcher();
}

static esp_err_t kit_exit(httpd_req_t *request)
{
    if (!appkit_authorized(request))
    {
        return appkit_unauthorized(request);
    }
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = exit_later, .name = "kit_exit"};
    if (timer == NULL && esp_timer_create(&args, &timer) != ESP_OK)
    {
        return appkit_error(request, "503 Service Unavailable", "Riprova");
    }
    esp_timer_start_once(timer, 400000);
    return appkit_json(request, cJSON_CreateObject());
}

/* ---------------------------------------------------------------- start */

static esp_err_t start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.max_uri_handlers = s_config->route_count + EXTRA_ROUTES;
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 8;
    config.send_wait_timeout = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK)
    {
        return err;
    }
    const httpd_uri_t own[EXTRA_ROUTES] = {
        {.uri = "/", .method = HTTP_GET, .handler = page},
        {.uri = "/kit.js", .method = HTTP_GET, .handler = kit_js},
        {.uri = "/kit.css", .method = HTTP_GET, .handler = kit_css},
        {.uri = "/api/pair", .method = HTTP_POST, .handler = pair},
        {.uri = "/api/kit", .method = HTTP_GET, .handler = kit_info},
        {.uri = "/api/kit/exit", .method = HTTP_POST, .handler = kit_exit},
    };
    for (int i = 0; i < EXTRA_ROUTES && err == ESP_OK; i++)
    {
        err = httpd_register_uri_handler(s_server, &own[i]);
    }
    for (size_t i = 0; i < s_config->route_count && err == ESP_OK; i++)
    {
        err = httpd_register_uri_handler(s_server, &s_config->routes[i]);
    }
    if (err != ESP_OK)
    {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return err;
}

static void web_task(void *arg)
{
    (void)arg;
    while (!atomic_load(&s_stop))
    {
        atomic_store(&s_state, APPKIT_WEB_JOINING);
        if (wifi_acquire(20000) == ESP_OK)
        {
            s_wifi_held = true;
            break;
        }
        atomic_store(&s_state, APPKIT_WEB_FAILED);
        for (int i = 0; i < 300 && !atomic_load(&s_stop); i++)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (atomic_load(&s_stop))
    {
        vTaskDelete(NULL);
        return;
    }

    char ip[20] = "";
    wifi_get_ip(ip, sizeof(ip));
    snprintf(s_url, sizeof(s_url), "http://%s/", ip);
    if (!s_mdns && mdns_init() == ESP_OK)
    {
        s_mdns = true;
        mdns_hostname_set(s_key);
        mdns_instance_name_set(s_config->name);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    esp_err_t err = start_server();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Server: %s", esp_err_to_name(err));
        atomic_store(&s_state, APPKIT_WEB_FAILED);
    }
    else
    {
        ESP_LOGI(TAG, "%s dashboard on %s (%s.local)", s_config->name, s_url, s_key);
        atomic_store(&s_state, APPKIT_WEB_ON);
    }
    vTaskDelete(NULL);
}

esp_err_t appkit_web_start(const appkit_web_config_t *config)
{
    appkit_json_hooks();
    if (s_config != NULL)
    {
        return ESP_OK;
    }
    s_config = config;
    size_t i = 0;
    for (; config->name[i] != '\0' && i < sizeof(s_key) - 1; i++)
    {
        s_key[i] = (char)tolower((unsigned char)config->name[i]);
    }
    s_key[i] = '\0';
    load_token();
    snprintf(s_code, sizeof(s_code), "%06lu", (unsigned long)(esp_random() % 1000000));
    atomic_store(&s_stop, false);
    if (!sys_task_create(web_task, "appkit_web", 4096, NULL, 4, SYS_CORE_ANY))
    {
        s_config = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void appkit_web_activate(void)
{
}

bool appkit_web_deactivate(void)
{
    return false;
}

bool appkit_web_deactivate_supported(void)
{
    return false;
}

void appkit_hosted_register(const char *name, const char *app_id)
{
    (void)name;
    (void)app_id;   /* external apps run their own server */
}

void appkit_web_host(char *out, size_t size)
{
    snprintf(out, size, "%s.local", s_key);
}

void appkit_web_stop(void)
{
    atomic_store(&s_stop, true);
    if (s_server != NULL)
    {
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_mdns)
    {
        mdns_free();
        s_mdns = false;
    }
    if (s_wifi_held)
    {
        wifi_release();
        s_wifi_held = false;
    }
    atomic_store(&s_state, APPKIT_WEB_OFF);
    s_config = NULL;
}
