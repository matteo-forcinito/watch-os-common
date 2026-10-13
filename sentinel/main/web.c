/* Local dashboard: pairing, header authentication and bounded import/export. */
#include "sentinel.h"
#include "packet.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#if CONFIG_WATCH_DEV_CONSOLE
#include "core/lv_port.h"
#endif

extern const char dashboard_start[] asm("_binary_dashboard_html_start");
extern const char dashboard_end[] asm("_binary_dashboard_html_end");
static httpd_handle_t s_server;
static char s_token[33], s_preview[65];
static int64_t s_rate_reset, s_preview_until;
static uint32_t s_preview_generation;
static int s_attempts;

static esp_err_t error(httpd_req_t *request, const char *status, const char *text)
{
    httpd_resp_set_status(request,status);
    httpd_resp_set_type(request,"text/plain; charset=utf-8");
    return httpd_resp_sendstr(request,text);
}

static bool same_origin(httpd_req_t *request)
{
    if (httpd_req_get_hdr_value_len(request,"Origin") == 0) return true;
    char origin[100], host[80], expected[100];
    if (httpd_req_get_hdr_value_str(request,"Origin",origin,sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(request,"Host",host,sizeof(host)) != ESP_OK) return false;
    snprintf(expected,sizeof(expected),"http://%s",host);
    return strcmp(origin,expected) == 0;
}

static bool authorized(httpd_req_t *request)
{
    char token[40];
    if (!same_origin(request) || httpd_req_get_hdr_value_str(request,"X-Sentinel-Key",token,sizeof(token)) != ESP_OK ||
        strlen(token) != 32 || !s_token[0]) return false;
    unsigned difference = 0;
    for (int i = 0; i < 32; i++) difference |= token[i] ^ s_token[i];
    return difference == 0;
}

static char *body(httpd_req_t *request, size_t limit)
{
    if (request->content_len <= 0 || request->content_len > limit) return NULL;
    char *text = heap_caps_malloc(request->content_len+1,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (text == NULL) return NULL;
    size_t got = 0;
    int timeouts = 0;
    while (got < request->content_len)
    {
        int result = httpd_req_recv(request,text+got,request->content_len-got);
        if (result == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (result <= 0) { free(text); return NULL; }
        got += result;
    }
    text[got] = 0;
    if (!sn_json_safe(text,got)) { free(text); return NULL; }
    return text;
}

static esp_err_t json_response(httpd_req_t *request, cJSON *json)
{
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (text == NULL) return error(request,"503 Service Unavailable","Memoria occupata");
    httpd_resp_set_type(request,"application/json");
    httpd_resp_set_hdr(request,"Cache-Control","no-store");
    httpd_resp_set_hdr(request,"X-Content-Type-Options","nosniff");
    esp_err_t result = httpd_resp_sendstr(request,text);
    free(text);
    return result;
}

static esp_err_t accepted(httpd_req_t *request, esp_err_t result)
{
    if (result != ESP_OK) return error(request,"409 Conflict","Operazione occupata o dati non validi");
    httpd_resp_set_status(request,"202 Accepted");
    return httpd_resp_sendstr(request,"Operazione avviata");
}

static esp_err_t page(httpd_req_t *request)
{
    httpd_resp_set_type(request,"text/html; charset=utf-8");
    httpd_resp_set_hdr(request,"Cache-Control","no-store");
    httpd_resp_set_hdr(request,"X-Frame-Options","DENY");
    httpd_resp_set_hdr(request,"Referrer-Policy","no-referrer");
    httpd_resp_set_hdr(request,"Content-Security-Policy",
        "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
    return httpd_resp_send(request,dashboard_start,dashboard_end-dashboard_start-1);
}

static esp_err_t pair(httpd_req_t *request)
{
    if (!same_origin(request)) return error(request,"403 Forbidden","Origine non consentita");
    int64_t now = esp_timer_get_time();
    if (now >= s_rate_reset) { s_attempts = 0; s_rate_reset = now+60000000; }
    if (++s_attempts > 5) return error(request,"429 Too Many Requests","Riprova tra un minuto");
    char *text = body(request,128);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root,"code");
    sn_status_t status;
    bool ok = cJSON_IsString(code) && sn_status(&status) && strcmp(code->valuestring,status.code) == 0;
    free(text); cJSON_Delete(root);
    if (!ok) return error(request,"401 Unauthorized","Codice errato");
    cJSON *out = cJSON_CreateObject();
    if (out) cJSON_AddStringToObject(out,"token",s_token);
    return json_response(request,out);
}

static esp_err_t status(httpd_req_t *request)
{
    if (!authorized(request)) return error(request,"401 Unauthorized","Abbina il dispositivo");
    sn_status_t s;
    if (!sn_status(&s)) return error(request,"503 Service Unavailable","Riprova");
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) return json_response(request,NULL);
    cJSON_AddStringToObject(out,"ip",s.ip); cJSON_AddStringToObject(out,"ssid",s.ssid);
    cJSON_AddStringToObject(out,"subnet",s.subnet); cJSON_AddStringToObject(out,"range",s.range);
    cJSON_AddStringToObject(out,"network",s.network); cJSON_AddStringToObject(out,"message",s.message);
    cJSON_AddStringToObject(out,"diagnostic",s.diagnostic);
    cJSON_AddNumberToObject(out,"devices",s.devices); cJSON_AddNumberToObject(out,"observed",s.observed);
    cJSON_AddNumberToObject(out,"new_devices",s.new_devices); cJSON_AddNumberToObject(out,"percent",s.percent);
    cJSON_AddNumberToObject(out,"rssi",s.rssi); cJSON_AddNumberToObject(out,"cycle",s.cycle);
    cJSON_AddNumberToObject(out,"channel",s.channel); cJSON_AddStringToObject(out,"security",s.security);
    cJSON_AddNumberToObject(out,"revision",s.revision); cJSON_AddNumberToObject(out,"interval",s.interval);
    cJSON_AddNumberToObject(out,"dropped",s.dropped);
    cJSON_AddBoolToObject(out,"busy",s.busy); cJSON_AddBoolToObject(out,"persistent",s.persistent);
    cJSON_AddBoolToObject(out,"dirty",s.dirty); cJSON_AddBoolToObject(out,"connected",s.connected);
    return json_response(request,out);
}

static sn_model_t *snapshot(void)
{
    sn_model_t *copy = heap_caps_malloc(sizeof(*copy),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return NULL;
    xSemaphoreTake(sn_mutex,portMAX_DELAY); *copy = *sn_model; xSemaphoreGive(sn_mutex);
    return copy;
}

static bool query_number(const char *query, const char *name, int *out)
{
    char value[16], *end;
    if (httpd_query_key_value(query,name,value,sizeof(value)) != ESP_OK) return true;
    long parsed = strtol(value,&end,10);
    if (*value == 0 || *end || parsed < 0 || parsed > SN_DEVICES) return false;
    *out = parsed; return true;
}

static bool decode_query(const char *encoded, char out[49])
{
    size_t used = 0;
    while (*encoded)
    {
        if (used >= 48) return false;
        if (*encoded == '%')
        {
            if (!isxdigit((unsigned char)encoded[1]) || !encoded[1] || !encoded[2] || !isxdigit((unsigned char)encoded[2])) return false;
            char pair[3] = {encoded[1],encoded[2],0};
            out[used++] = (char)strtol(pair,NULL,16); encoded += 3;
        }
        else out[used++] = *encoded == '+' ? (encoded++, ' ') : *encoded++;
    }
    out[used] = 0;
    return strlen(out) == used && sn_text_valid(out,48);
}

static bool contains(const char *text, const char *query)
{
    if (!*query) return true;
    for (const char *start = text; *start; start++)
    {
        const unsigned char *a = (const unsigned char *)start, *b = (const unsigned char *)query;
        while (*a && *b && tolower(*a) == tolower(*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

static esp_err_t devices(httpd_req_t *request)
{
    if (!authorized(request)) return error(request,"401 Unauthorized","Abbina il dispositivo");
    char query[384] = "", network[34] = "", encoded[160] = "", search[49] = "";
    if (httpd_req_get_url_query_len(request) >= sizeof(query)) return error(request,"400 Bad Request","Ricerca troppo lunga");
    httpd_req_get_url_query_str(request,query,sizeof(query));
    int offset = 0, limit = 40;
    if (!query_number(query,"offset",&offset) || !query_number(query,"limit",&limit) || limit < 1 || limit > 50)
        return error(request,"400 Bad Request","Pagina non valida");
    httpd_query_key_value(query,"network",network,sizeof(network));
    if (httpd_query_key_value(query,"q",encoded,sizeof(encoded)) == ESP_OK && !decode_query(encoded,search))
        return error(request,"400 Bad Request","Ricerca non valida (massimo 48 byte)");
    sn_model_t *copy = snapshot();
    cJSON *root = cJSON_CreateObject();
    cJSON *items = root ? cJSON_AddArrayToObject(root,"devices") : NULL;
    cJSON *networks = root ? cJSON_AddArrayToObject(root,"networks") : NULL;
    if (!copy || !items || !networks) { free(copy); return json_response(request,root); }
    int matched = 0, included = 0;
    for (int i = 0; i < copy->device_count; i++)
    {
        sn_device_t *d = &copy->devices[i];
        if (*network && strcmp(network,d->network) != 0) continue;
        if (!contains(d->label,search) && !contains(d->hostname,search) && !contains(d->ip,search) &&
            !contains(d->mac,search) && !contains(d->category,search) && !contains(d->services,search)) continue;
        if (matched++ < offset || included >= limit) continue;
        cJSON *item = sn_device_json(d);
        if (item == NULL) { free(copy); cJSON_Delete(root); return json_response(request,NULL); }
        cJSON_AddBoolToObject(item,"observed",copy->cycle != 0 && d->seen_cycle == copy->cycle);
        cJSON_AddBoolToObject(item,"confirmed",sn_confirmed(d));
        cJSON_AddBoolToObject(item,"new_device",copy->cycle != 0 && d->new_cycle == copy->cycle);
        cJSON_AddItemToArray(items,item); included++;
    }
    for (int i = 0; i < copy->network_count; i++) cJSON_AddItemToArray(networks,sn_network_json(&copy->networks[i]));
    cJSON_AddNumberToObject(root,"total",matched); cJSON_AddNumberToObject(root,"offset",offset);
    free(copy);
    return json_response(request,root);
}

static esp_err_t action(httpd_req_t *request)
{
    if (!authorized(request)) return error(request,"401 Unauthorized","Abbina il dispositivo");
    if (strcmp(request->uri,"/api/cancel") == 0)
    { sn_cancel(); return httpd_resp_sendstr(request,"Arresto richiesto"); }
    char *text = body(request,1600);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!root) return error(request,"400 Bad Request","JSON non valido");
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (strcmp(request->uri,"/api/scan") == 0)
    {
        cJSON *range = cJSON_GetObjectItemCaseSensitive(root,"range");
        if (range == NULL || cJSON_IsString(range)) result = sn_scan(range ? range->valuestring : "");
    }
    else if (strcmp(request->uri,"/api/diagnose") == 0)
    {
        cJSON *ip = cJSON_GetObjectItemCaseSensitive(root,"ip");
        if (cJSON_IsString(ip)) result = sn_diagnose(ip->valuestring);
    }
    else if (strcmp(request->uri,"/api/settings") == 0)
    {
        cJSON *interval = cJSON_GetObjectItemCaseSensitive(root,"interval");
        if (cJSON_IsNumber(interval) && interval->valuedouble == interval->valueint && interval->valueint >= 0)
            result = sn_settings(interval->valueint);
    }
    else if (strcmp(request->uri,"/api/edit") == 0)
    {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(root,"id");
        if (cJSON_IsString(id) && strlen(id->valuestring) == 32)
        {
            sn_device_t device;
            sn_status_t s;
            if (sn_status(&s)) for (int i = 0; i < s.devices; i++)
            {
                if (!sn_device_at(i,&device) || strcmp(device.id,id->valuestring) != 0) continue;
                cJSON *label = cJSON_GetObjectItemCaseSensitive(root,"label");
                cJSON *category = cJSON_GetObjectItemCaseSensitive(root,"category");
                cJSON *note = cJSON_GetObjectItemCaseSensitive(root,"note");
                cJSON *trusted = cJSON_GetObjectItemCaseSensitive(root,"trusted");
                if (!cJSON_IsString(label) || !cJSON_IsString(category) || !cJSON_IsString(note) || !cJSON_IsBool(trusted) ||
                    !sn_text_valid(label->valuestring,64) || !sn_text_valid(category->valuestring,32) || !sn_text_valid(note->valuestring,256)) break;
                snprintf(device.label,sizeof(device.label),"%s",label->valuestring);
                snprintf(device.category,sizeof(device.category),"%s",category->valuestring);
                snprintf(device.note,sizeof(device.note),"%s",note->valuestring);
                device.trusted = cJSON_IsTrue(trusted);
                result = sn_edit(&device); break;
            }
        }
    }
    cJSON_Delete(root);
    return accepted(request,result);
}

static esp_err_t export_json(httpd_req_t *request)
{
    if (!authorized(request)) return error(request,"401 Unauthorized","Abbina il dispositivo");
    char *json = sn_export();
    if (!json) return error(request,"503 Service Unavailable","Memoria occupata");
    httpd_resp_set_type(request,"application/json");
    httpd_resp_set_hdr(request,"Cache-Control","no-store");
    httpd_resp_set_hdr(request,"Content-Disposition","attachment; filename=sentinel-inventory.json");
    esp_err_t result = httpd_resp_sendstr(request,json); free(json); return result;
}

static esp_err_t export_csv(httpd_req_t *request)
{
    if (!authorized(request)) return error(request,"401 Unauthorized","Abbina il dispositivo");
    sn_model_t *copy = snapshot();
    if (!copy) return error(request,"503 Service Unavailable","Memoria occupata");
    httpd_resp_set_type(request,"text/csv; charset=utf-8");
    httpd_resp_set_hdr(request,"Content-Disposition","attachment; filename=sentinel-inventory.csv");
    esp_err_t result = httpd_resp_sendstr_chunk(request,"network,id,label,category,ip,mac,hostname,services,first_seen,last_seen,trusted\r\n");
    for (int i = 0; i < copy->device_count && result == ESP_OK; i++)
    {
        const sn_device_t *d = &copy->devices[i];
        const char *fields[] = {d->network,d->id,d->label,d->category,d->ip,d->mac,d->hostname,d->services};
        char line[1500]; size_t used = 0;
        for (unsigned field = 0; field < sizeof(fields)/sizeof(fields[0]); field++)
        {
            const char *p = fields[field];
            line[used++] = '"';
            if (*p && strchr("=+-@",*p)) line[used++] = '\'';
            while (*p && used < sizeof(line)-100)
            { if (*p == '"') line[used++] = '"'; line[used++] = *p++; }
            line[used++] = '"'; line[used++] = ',';
        }
        snprintf(line+used,sizeof(line)-used,"%lld,%lld,%s\r\n",(long long)d->first_seen,(long long)d->last_seen,d->trusted ? "true" : "false");
        result = httpd_resp_sendstr_chunk(request,line);
    }
    free(copy);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request,NULL,0);
    return result;
}

static bool hash(const char *text, char out[65])
{
    unsigned char digest[32];
    if (!sn_digest(text,strlen(text),digest)) return false;
    for (int i = 0; i < 32; i++) snprintf(out+i*2,3,"%02x",digest[i]);
    return true;
}

static esp_err_t import_json(httpd_req_t *request)
{
    if (!authorized(request)) return error(request,"401 Unauthorized","Abbina il dispositivo");
    sn_status_t status;
    if (!sn_status(&status) || status.busy || status.dirty)
        return error(request,"409 Conflict","Attendi operazione e salvataggio prima di importare");
    char *text = body(request,SN_FILE_LIMIT);
    if (!text) return error(request,"400 Bad Request","Archivio troppo grande o incompleto");
    char digest[65], why[96];
    if (!hash(text,digest)) { free(text); return error(request,"503 Service Unavailable","Hash non disponibile"); }
    sn_model_t *incoming = sn_parse(text,strlen(text),why,sizeof(why));
    free(text);
    if (!incoming) return error(request,"400 Bad Request",why);
    if (strcmp(request->uri,"/api/import/preview") == 0)
    {
        snprintf(s_preview,sizeof(s_preview),"%s",digest);
        s_preview_until = esp_timer_get_time()+300000000;
        xSemaphoreTake(sn_mutex,portMAX_DELAY); s_preview_generation = sn_model->generation; xSemaphoreGive(sn_mutex);
        cJSON *out = cJSON_CreateObject();
        if (out)
        {
            cJSON_AddStringToObject(out,"digest",digest);
            cJSON_AddNumberToObject(out,"devices",incoming->device_count);
            cJSON_AddNumberToObject(out,"networks",incoming->network_count);
            cJSON_AddStringToObject(out,"policy","Unione; nomi e dati esistenti conservati. Nessuna cancellazione.");
        }
        free(incoming);
        return json_response(request,out);
    }
    char expected[66];
    xSemaphoreTake(sn_mutex,portMAX_DELAY); bool current = s_preview_generation == sn_model->generation; xSemaphoreGive(sn_mutex);
    if (!current || esp_timer_get_time() >= s_preview_until || strcmp(s_preview,digest) != 0 ||
        httpd_req_get_hdr_value_str(request,"X-Import-Digest",expected,sizeof(expected)) != ESP_OK || strcmp(expected,digest) != 0)
    { free(incoming); return error(request,"409 Conflict","Rifai l'anteprima dell'archivio"); }
    esp_err_t result = sn_import(incoming);
    if (result != ESP_OK) free(incoming);
    else s_preview_until = 0;
    return accepted(request,result);
}

#if CONFIG_WATCH_DEV_CONSOLE
typedef struct { lv_draw_buf_t *buffer; SemaphoreHandle_t done; } screen_job_t;
static void capture_screen(void *argument)
{
    screen_job_t *job = argument;
    job->buffer = lv_snapshot_take(lv_screen_active(),LV_COLOR_FORMAT_RGB565);
    xSemaphoreGive(job->done);
}
/* Development-only binary transport, avoiding lossy console base64 output. */
static esp_err_t qa_screen(httpd_req_t *request)
{
    if (!authorized(request)) return error(request,"401 Unauthorized","Abbina il dispositivo");
    screen_job_t job = {.done=xSemaphoreCreateBinary()};
    if (!job.done) return error(request,"503 Service Unavailable","Memoria occupata");
    if (ui_async(capture_screen,&job)) xSemaphoreTake(job.done,portMAX_DELAY);
    vSemaphoreDelete(job.done);
    if (!job.buffer) return error(request,"503 Service Unavailable","Snapshot non disponibile");
    char width[16], height[16], stride[16];
    snprintf(width,sizeof(width),"%lu",(unsigned long)job.buffer->header.w);
    snprintf(height,sizeof(height),"%lu",(unsigned long)job.buffer->header.h);
    snprintf(stride,sizeof(stride),"%lu",(unsigned long)job.buffer->header.stride);
    httpd_resp_set_hdr(request,"X-Screen-Width",width);
    httpd_resp_set_hdr(request,"X-Screen-Height",height);
    httpd_resp_set_hdr(request,"X-Screen-Stride",stride);
    httpd_resp_set_type(request,"application/octet-stream");
    esp_err_t result = httpd_resp_send(request,(char *)job.buffer->data,job.buffer->header.stride*job.buffer->header.h);
    lv_draw_buf_destroy(job.buffer);
    return result;
}
#endif

esp_err_t sn_web_start(void)
{
    if (s_server) return ESP_OK;
    uint8_t bytes[16]; esp_fill_random(bytes,sizeof(bytes));
    for (int i = 0; i < 16; i++) snprintf(s_token+i*2,3,"%02x",bytes[i]);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; config.max_uri_handlers = 16; config.max_open_sockets = 4;
    config.lru_purge_enable = true; config.recv_wait_timeout = 5; config.send_wait_timeout = 5;
    esp_err_t result = httpd_start(&s_server,&config);
    if (result != ESP_OK) return result;
    const httpd_uri_t routes[] = {
        {.uri="/",.method=HTTP_GET,.handler=page},
        {.uri="/api/pair",.method=HTTP_POST,.handler=pair},
        {.uri="/api/status",.method=HTTP_GET,.handler=status},
        {.uri="/api/devices",.method=HTTP_GET,.handler=devices},
        {.uri="/api/export",.method=HTTP_GET,.handler=export_json},
        {.uri="/api/export.csv",.method=HTTP_GET,.handler=export_csv},
        {.uri="/api/scan",.method=HTTP_POST,.handler=action},
        {.uri="/api/cancel",.method=HTTP_POST,.handler=action},
        {.uri="/api/diagnose",.method=HTTP_POST,.handler=action},
        {.uri="/api/edit",.method=HTTP_POST,.handler=action},
        {.uri="/api/settings",.method=HTTP_POST,.handler=action},
        {.uri="/api/import/preview",.method=HTTP_POST,.handler=import_json},
        {.uri="/api/import",.method=HTTP_POST,.handler=import_json}
#if CONFIG_WATCH_DEV_CONSOLE
        ,{.uri="/api/qa/screen",.method=HTTP_GET,.handler=qa_screen}
#endif
    };
    for (unsigned i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        if ((result = httpd_register_uri_handler(s_server,&routes[i])) != ESP_OK)
        { sn_web_stop(); return result; }
    return ESP_OK;
}
void sn_web_stop(void)
{
    if (s_server) httpd_stop(s_server);
    s_server = NULL;
    memset(s_token,0,sizeof(s_token));
}
