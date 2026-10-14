/*
 * Route helpers shared by the two dashboard servers (see appkit.h):
 * appkit_web.c (an external app's own server) and appkit_hosted.c (an
 * internal app's routes on the OS web server).
 */
#include "appkit.h"
#include "appkit_private.h"

#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

esp_err_t appkit_error(httpd_req_t *request, const char *status, const char *text)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, text);
}

esp_err_t appkit_unauthorized(httpd_req_t *request)
{
    return appkit_error(request, "401 Unauthorized", "Abbina la dashboard");
}

char *appkit_body(httpd_req_t *request, size_t limit)
{
    if (request->content_len == 0 || request->content_len > limit)
    {
        return NULL;
    }
    char *text = heap_caps_malloc(request->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (text == NULL)
    {
        return NULL;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < request->content_len)
    {
        int result = httpd_req_recv(request, text + got, request->content_len - got);
        if (result == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3)
        {
            continue;
        }
        if (result <= 0)
        {
            free(text);
            return NULL;
        }
        got += (size_t)result;
    }
    text[got] = '\0';
    return text;
}

esp_err_t appkit_json(httpd_req_t *request, cJSON *json)
{
    char *text = json != NULL ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (text == NULL)
    {
        return appkit_error(request, "503 Service Unavailable", "Memoria occupata, riprova");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    esp_err_t result = httpd_resp_sendstr(request, text);
    cJSON_free(text);
    return result;
}

bool appkit_query(httpd_req_t *request, const char *key, char *out, size_t size)
{
    out[0] = '\0';
    size_t length = httpd_req_get_url_query_len(request);
    if (length == 0 || length > 512)
    {
        return false;
    }
    char query[513];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK)
    {
        return false;
    }
    return httpd_query_key_value(query, key, out, size) == ESP_OK;
}

/* cJSON nodes are small: by default they land in the scarce internal RAM. Keep them in PSRAM. */
static void *json_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void appkit_json_hooks(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
}

