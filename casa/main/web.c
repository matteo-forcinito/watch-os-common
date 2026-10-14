/*
 * Casa setup page (see casa.h). Served while Casa runs; the watch shows the
 * address and a 6-digit code. The browser talks to Home Assistant directly to
 * list areas and entities; the watch only receives URL, token and favourites.
 * Every API call carries X-Casa-Code and must come from the page itself.
 * The token is never sent back to the browser.
 */
#include "casa.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "services/storage.h"
#include "services/wifi.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "casa_web";

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");

static httpd_handle_t s_server;
static char s_code[8], s_address[48];

static bool authorized(httpd_req_t *request)
{
    if (httpd_req_get_hdr_value_len(request, "Origin") > 0)
    {
        char origin[100], host[80], expected[100];
        if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK ||
            httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK)
            return false;
        snprintf(expected, sizeof(expected), "http://%s", host);
        if (strcmp(origin, expected) != 0)
            return false;
    }
    char code[12];
    if (httpd_req_get_hdr_value_str(request, "X-Casa-Code", code, sizeof(code)) != ESP_OK)
        return false;
    unsigned difference = strlen(code) != strlen(s_code);
    for (size_t i = 0; i < strlen(s_code) && i < strlen(code); i++)
        difference |= (unsigned)(code[i] ^ s_code[i]);
    return difference == 0;
}

static esp_err_t send_json(httpd_req_t *request, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t error = httpd_resp_sendstr(request, text ? text : "{}");
    free(text);
    return error;
}

static esp_err_t refuse(httpd_req_t *request)
{
    httpd_resp_set_status(request, "403 Forbidden");
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", false);
    cJSON_AddStringToObject(reply, "error", "Codice errato: guarda l'orologio");
    return send_json(request, reply);
}

static esp_err_t page(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    /* EMBED_TXTFILES adds a terminating zero: not part of the page. */
    return httpd_resp_send(request, setup_html_start, setup_html_end - setup_html_start - 1);
}

static esp_err_t get_config(httpd_req_t *request)
{
    if (!authorized(request))
        return refuse(request);
    cJSON *reply = cJSON_CreateObject();
    char url[160] = "";
    bool has_token = false;
    nvs_handle_t nvs;
    if (nvs_open("casa", NVS_READONLY, &nvs) == ESP_OK)
    {
        size_t size = sizeof(url);
        nvs_get_str(nvs, "url", url, &size);
        size = 0;
        has_token = nvs_get_str(nvs, "token", NULL, &size) == ESP_OK && size > 1;
        nvs_close(nvs);
    }
    cJSON_AddStringToObject(reply, "url", url);
    cJSON_AddBoolToObject(reply, "has_token", has_token);
    cJSON *favorites = cJSON_AddArrayToObject(reply, "favorites");
    casa_entity_t *list = heap_caps_calloc(CASA_ENTITIES, sizeof(casa_entity_t), MALLOC_CAP_SPIRAM);
    int count = list ? casa_entities(list, CASA_ENTITIES) : 0;
    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", list[i].id);
        cJSON_AddStringToObject(item, "name", list[i].name);
        cJSON_AddStringToObject(item, "area", list[i].area);
        cJSON_AddItemToArray(favorites, item);
    }
    free(list);
    return send_json(request, reply);
}

static esp_err_t post_config(httpd_req_t *request)
{
    if (!authorized(request))
        return refuse(request);
    if (request->content_len <= 0 || request->content_len > 24 * 1024)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Troppo grande");
    char *body = heap_caps_malloc(request->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL)
        return httpd_resp_send_500(request);
    int received = 0;
    while (received < request->content_len)
    {
        int n = httpd_req_recv(request, body + received, request->content_len - received);
        if (n <= 0)
        {
            free(body);
            return ESP_FAIL;
        }
        received += n;
    }
    body[received] = 0;
    char error[120] = "";
    esp_err_t stored = casa_config_store(body, error, sizeof(error));
    memset(body, 0, received); /* it held the token */
    free(body);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", stored == ESP_OK);
    if (stored != ESP_OK)
        cJSON_AddStringToObject(reply, "error", error);
    else
        ESP_LOGI(TAG, "Configuration saved");
    return send_json(request, reply);
}

esp_err_t casa_web_start(void)
{
    if (s_server)
        return ESP_OK;
    if (!s_code[0])
        snprintf(s_code, sizeof(s_code), "%06lu", (unsigned long)(esp_random() % 1000000));
    if (!wifi_get_ip(s_address, sizeof(s_address)))
        snprintf(s_address, sizeof(s_address), "?");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_uri_handlers = 4;
    /* Internal-RAM stack: saving writes flash (NVS, FAT), which needs a stack the cache cannot hide. */
    esp_err_t error = httpd_start(&s_server, &config);
    if (error != ESP_OK)
        return error;
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page},
        {.uri = "/api/config", .method = HTTP_GET, .handler = get_config},
        {.uri = "/api/config", .method = HTTP_POST, .handler = post_config},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(s_server, &routes[i]);
#if CONFIG_WATCH_DEV_CONSOLE
    ESP_LOGI(TAG, "Setup page at http://%s, code %s (QA log)", s_address, s_code);
#else
    ESP_LOGI(TAG, "Setup page at http://%s", s_address);
#endif
    return ESP_OK;
}

const char *casa_web_code(void) { return s_code; }
const char *casa_web_address(void) { return s_address; }
