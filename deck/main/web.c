/*
 * Deck configuration mode: the editor page on the PC.
 *
 * Wi-Fi only (the HID stacks are not started in this mode). The watch shows
 * its address and a 6-digit code; every API call carries the code in
 * X-Deck-Code and must come from the page itself (same Origin), so another
 * page in the browser cannot rewrite the keys. Plain HTTP on the local
 * network: the profiles hold shortcuts, keep secrets out of them.
 */
#include "deck.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "services/wifi.h"
#include "sdkconfig.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "deck_web";

extern const char config_html_start[] asm("_binary_config_html_start");
extern const char config_html_end[] asm("_binary_config_html_end");

static httpd_handle_t s_server;
static char s_code[8], s_address[48];
static atomic_bool s_saved;

static bool same_origin(httpd_req_t *request)
{
    if (httpd_req_get_hdr_value_len(request, "Origin") == 0)
        return true;
    char origin[100], host[80], expected[100];
    if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK)
        return false;
    snprintf(expected, sizeof(expected), "http://%s", host);
    return strcmp(origin, expected) == 0;
}

static bool authorized(httpd_req_t *request)
{
    char code[12];
    if (!same_origin(request) || httpd_req_get_hdr_value_str(request, "X-Deck-Code", code, sizeof(code)) != ESP_OK)
        return false;
    unsigned difference = strlen(code) != strlen(s_code);
    for (size_t i = 0; i < strlen(s_code) && i < strlen(code); i++)
        difference |= (unsigned)(code[i] ^ s_code[i]);
    return difference == 0;
}

static esp_err_t refuse(httpd_req_t *request)
{
    httpd_resp_set_status(request, "403 Forbidden");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":false,\"error\":\"Codice errato: guarda l'orologio\"}");
}

static esp_err_t page(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    /* EMBED_TXTFILES adds a terminating zero: not part of the page. */
    return httpd_resp_send(request, config_html_start, config_html_end - config_html_start - 1);
}

static esp_err_t get_profiles(httpd_req_t *request)
{
    if (!authorized(request))
        return refuse(request);
    char *json = deck_profiles_json();
    if (json == NULL)
        return httpd_resp_send_500(request);
    httpd_resp_set_type(request, "application/json");
    esp_err_t error = httpd_resp_sendstr(request, json);
    free(json);
    return error;
}

static esp_err_t post_profiles(httpd_req_t *request)
{
    if (!authorized(request))
        return refuse(request);
    if (request->content_len <= 0 || request->content_len > 32 * 1024)
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
    char error[128] = "";
    esp_err_t stored = deck_profiles_store(body, error, sizeof(error));
    free(body);
    httpd_resp_set_type(request, "application/json");
    if (stored != ESP_OK)
    {
        /* The message quotes the key label: let cJSON escape it. */
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddBoolToObject(reply, "ok", false);
        cJSON_AddStringToObject(reply, "error", error);
        char *text = cJSON_PrintUnformatted(reply);
        cJSON_Delete(reply);
        esp_err_t sent = httpd_resp_sendstr(request, text ? text : "{\"ok\":false}");
        free(text);
        return sent;
    }
    atomic_store(&s_saved, true);
    ESP_LOGI(TAG, "Custom profiles saved");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

esp_err_t deck_web_start(void)
{
    snprintf(s_code, sizeof(s_code), "%06lu", (unsigned long)(esp_random() % 1000000));
    esp_err_t error = wifi_acquire(20000);
    if (error != ESP_OK)
        return error;
    if (!wifi_get_ip(s_address, sizeof(s_address)))
        snprintf(s_address, sizeof(s_address), "?");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_uri_handlers = 4;
    /* Internal-RAM stack: saving writes flash (NVS, FAT), which needs a stack the cache cannot hide. */
    if ((error = httpd_start(&s_server, &config)) != ESP_OK)
        return error;
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page},
        {.uri = "/api/profiles", .method = HTTP_GET, .handler = get_profiles},
        {.uri = "/api/profiles", .method = HTTP_POST, .handler = post_profiles},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(s_server, &routes[i]);
#if CONFIG_WATCH_DEV_CONSOLE
    ESP_LOGI(TAG, "Editor at http://%s, code %s (QA log)", s_address, s_code);
#else
    ESP_LOGI(TAG, "Editor at http://%s", s_address);
#endif
    return ESP_OK;
}

const char *deck_web_code(void) { return s_code; }
const char *deck_web_address(void) { return s_address; }
bool deck_web_saved(void) { return atomic_load(&s_saved); }
