/* Optional HTTPS-only dashboard. No cloud, cookies, logs of secrets or CDN.
 * TLS identity is per session; compare its SHA-256 fingerprint on the watch. */
#include "vault.h"
#include "services/wifi.h"
#include "core/power.h"
#include "core/clock.h"
#include "esp_https_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include "mbedtls/base64.h"
#include "lwip/inet.h"
#include "packet.h"
#include <stdio.h>
#include <string.h>

static httpd_handle_t s_server;
#if VAULT_QA
#define QA_FAIL(step, error) printf("@VAULTWEBFAIL %s %s\n", step, esp_err_to_name(error))
#else
#define QA_FAIL(step, error) ((void)0)
#endif
static unsigned char *s_certificate, *s_private;
static char s_url[64], s_code[9], s_token[65], s_fingerprint[96], s_host[32];
static bool s_wifi;
static unsigned s_attempts;
static int64_t s_pair_window;
static portMUX_TYPE s_info = portMUX_INITIALIZER_UNLOCKED;
extern const char vault_html_start[] asm("_binary_dashboard_html_start");
extern const char vault_html_end[] asm("_binary_dashboard_html_end");
extern const char vault_js_start[] asm("_binary_dashboard_js_start");
extern const char vault_js_end[] asm("_binary_dashboard_js_end");

void vault_web_info(char *url, size_t url_size, char *code, size_t code_size, char *fingerprint,
                    size_t fingerprint_size)
{
    portENTER_CRITICAL(&s_info);
    snprintf(url, url_size, "%s", s_url);
    snprintf(code, code_size, "%s", s_code);
    snprintf(fingerprint, fingerprint_size, "%s", s_fingerprint);
    portEXIT_CRITICAL(&s_info);
}

static esp_err_t error(httpd_req_t *request, const char *status, const char *message)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, message);
}

static bool origin(httpd_req_t *request)
{
    char host[48], value[96], expected_host[sizeof(s_host)], expected_url[sizeof(s_url)];
    portENTER_CRITICAL(&s_info);
    snprintf(expected_host, sizeof(expected_host), "%s", s_host);
    snprintf(expected_url, sizeof(expected_url), "%s", s_url);
    portEXIT_CRITICAL(&s_info);
    size_t length = httpd_req_get_hdr_value_len(request, "Host");
    if (length == 0 || length >= sizeof(host) ||
        httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK)
        return false;
    char with_port[sizeof(s_host) + 8];
    snprintf(with_port, sizeof(with_port), "%s:443", expected_host);
    if (strcmp(host, expected_host) != 0 && strcmp(host, with_port) != 0)
        return false;
    length = httpd_req_get_hdr_value_len(request, "Origin");
    if (length == 0)
        return true;
    if (length >= sizeof(value) ||
        httpd_req_get_hdr_value_str(request, "Origin", value, sizeof(value)) != ESP_OK)
        return false;
    return strcmp(value, expected_url) == 0;
}

static bool authorized(httpd_req_t *request)
{
    if (!vault_unlocked() || !origin(request))
        return false;
    char token[65] = {0}, expected[65];
    if (httpd_req_get_hdr_value_len(request, "X-Vault-Key") != 64 ||
        httpd_req_get_hdr_value_str(request, "X-Vault-Key", token, sizeof(token)) != ESP_OK)
        return false;
    portENTER_CRITICAL(&s_info);
    memcpy(expected, s_token, sizeof(expected));
    portEXIT_CRITICAL(&s_info);
    bool result = expected[0] != 0 && vault_equal(token, expected, 65);
    vault_wipe(token, sizeof(token));
    vault_wipe(expected, sizeof(expected));
    return result;
}

static esp_err_t json_response(httpd_req_t *request, cJSON *object)
{
    char *json = object != NULL ? cJSON_PrintUnformatted(object) : NULL;
    cJSON_Delete(object);
    if (json == NULL)
        return error(request, "503 Service Unavailable", "{\"error\":\"memoria\"}");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = vault_unlocked() ? httpd_resp_sendstr(request, json)
                                        : error(request, "423 Locked", "{\"error\":\"bloccato\"}");
    vault_secure_free(json);
    return result;
}

static uint8_t *body(httpd_req_t *request, size_t maximum)
{
    if (request->content_len == 0 || request->content_len > maximum)
        return NULL;
    uint8_t *data = vault_secure_alloc(request->content_len + 1);
    if (data == NULL)
        return NULL;
    size_t used = 0;
    int64_t deadline = esp_timer_get_time() + 15000000;
    while (used < request->content_len && esp_timer_get_time() < deadline && vault_unlocked())
    {
        int received = httpd_req_recv(request, (char *)data + used, request->content_len - used);
        if (received == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (received <= 0)
            break;
        used += received;
    }
    if (used != request->content_len)
    {
        vault_secure_free(data);
        return NULL;
    }
    data[used] = 0;
    return data;
}

static cJSON *json_body(httpd_req_t *request)
{
    char type[40];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") != 0)
        return NULL;
    uint8_t *bytes = body(request, 32 * 1024);
    if (bytes == NULL)
        return NULL;
    const char *end = NULL;
    cJSON *json =
        sn_json_safe((const char *)bytes, request->content_len)
            ? cJSON_ParseWithLengthOpts((const char *)bytes, request->content_len, &end, false)
            : NULL;
    while (end != NULL && end < (const char *)bytes + request->content_len &&
           (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t'))
        end++;
    if (end != (const char *)bytes + request->content_len)
    {
        cJSON_Delete(json);
        json = NULL;
    }
    vault_secure_free(bytes);
    return json;
}

static esp_err_t asset(httpd_req_t *request)
{
    if (!origin(request))
        return error(request, "403 Forbidden", "{}");
    bool script = strcmp(request->uri, "/dashboard.js") == 0;
    httpd_resp_set_type(request, script ? "application/javascript" : "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(
        request, "Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; connect-src 'self'; "
        "frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    return httpd_resp_send(request, script ? vault_js_start : vault_html_start,
                           script ? vault_js_end - vault_js_start - 1
                                  : vault_html_end - vault_html_start - 1);
}

static esp_err_t pair(httpd_req_t *request)
{
    if (!vault_unlocked() || !origin(request))
        return error(request, "403 Forbidden", "{}");
    int64_t now = esp_timer_get_time();
    if (now - s_pair_window > 60000000)
    {
        s_pair_window = now;
        s_attempts = 0;
    }
    if (++s_attempts > 5)
        return error(request, "429 Too Many Requests", "{\"error\":\"attendi\"}");
    cJSON *json = json_body(request), *code = cJSON_GetObjectItemCaseSensitive(json, "code");
    char expected[9];
    portENTER_CRITICAL(&s_info);
    memcpy(expected, s_code, 9);
    portEXIT_CRITICAL(&s_info);
    bool valid = cJSON_IsString(code) && strlen(code->valuestring) == 8 && expected[0] &&
                 vault_equal(code->valuestring, expected, 9);
    cJSON_Delete(json);
    vault_wipe(expected, sizeof(expected));
    if (!valid)
        return error(request, "401 Unauthorized", "{\"error\":\"codice\"}");
    uint8_t random[32];
    if (vault_random(random, 32) != ESP_OK)
        return error(request, "503 Service Unavailable", "{}");
    char token[65];
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++)
    {
        token[2 * i] = hex[random[i] >> 4];
        token[2 * i + 1] = hex[random[i] & 15];
    }
    token[64] = 0;
    portENTER_CRITICAL(&s_info);
    memcpy(s_token, token, 65);
    vault_wipe(s_code, 9);
    portEXIT_CRITICAL(&s_info);
    vault_wipe(random, sizeof(random));
    cJSON *reply = cJSON_CreateObject();
    if (reply != NULL && cJSON_AddStringToObject(reply, "token", token) == NULL)
    {
        cJSON_Delete(reply);
        reply = NULL;
    }
    vault_wipe(token, sizeof(token));
    vault_activity();
    return json_response(request, reply);
}

static esp_err_t submit(httpd_req_t *request, vault_job_t *job)
{
    if (job == NULL)
        return error(request, "503 Service Unavailable", "{}");
    esp_err_t result = vault_submit(job);
    if (result != ESP_OK)
        vault_job_free(job);
    if (result != ESP_OK)
        return error(request, "409 Conflict", "{\"error\":\"attendi o aggiorna la pagina\"}");
    httpd_resp_set_status(request, "202 Accepted");
    return error(request, "202 Accepted", "{\"accepted\":true}");
}

static vault_job_t *job(vault_operation_t operation)
{
    vault_job_t *result = vault_secure_alloc(sizeof(*result));
    if (result != NULL)
    {
        memset(result, 0, sizeof(*result));
        result->operation = operation;
    }
    return result;
}

static bool generation(const cJSON *json, uint64_t *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, "generation");
    if (!cJSON_IsString(item) || !item->valuestring[0])
        return false;
    uint64_t number = 0;
    for (const unsigned char *p = (const unsigned char *)item->valuestring; *p; p++)
    {
        if (*p < '0' || *p > '9' || number > (UINT64_MAX - (*p - '0')) / 10)
            return false;
        number = number * 10 + (*p - '0');
    }
    *value = number;
    return true;
}

static esp_err_t api(httpd_req_t *request)
{
    if (!authorized(request))
        return error(request, "401 Unauthorized", "{\"error\":\"sessione scaduta\"}");
    const char *route = request->uri;
    if (strcmp(route, "/api/status") == 0)
    {
        vault_status_t status;
        if (!vault_status(&status))
            return error(request, "503 Service Unavailable", "{}");
        cJSON *reply = cJSON_CreateObject();
        char gen[24];
        snprintf(gen, sizeof(gen), "%llu", (unsigned long long)status.generation);
        if (reply == NULL || !cJSON_AddBoolToObject(reply, "busy", status.busy) ||
            !cJSON_AddNumberToObject(reply, "revision", status.revision) ||
            !cJSON_AddNumberToObject(reply, "count", status.count) ||
            !cJSON_AddNumberToObject(reply, "preview_count", status.preview_count) ||
            !cJSON_AddStringToObject(reply, "message", status.message) ||
            !cJSON_AddNumberToObject(reply, "error", status.last_error) ||
            !cJSON_AddStringToObject(reply, "generation", gen))
        {
            cJSON_Delete(reply);
            reply = NULL;
        }
        return json_response(request, reply);
    }
    if (strcmp(route, "/api/entries") == 0)
    {
        vault_model_t *model = vault_snapshot();
        cJSON *reply = cJSON_CreateObject(),
              *entries = reply != NULL ? cJSON_AddArrayToObject(reply, "entries") : NULL;
        bool ok = model != NULL && entries != NULL;
        for (unsigned i = 0; ok && i < model->count; i++)
        {
            cJSON *entry = vault_entry_json(&model->entries[i], false);
            if (entry == NULL || !cJSON_AddItemToArray(entries, entry))
            {
                cJSON_Delete(entry);
                ok = false;
            }
        }
        vault_secure_free(model);
        if (!ok)
        {
            cJSON_Delete(reply);
            reply = NULL;
        }
        return json_response(request, reply);
    }
    if (strcmp(route, "/api/preview") == 0)
    {
        char token[33];
        int *conflicts = vault_secure_alloc(sizeof(int) * VAULT_ENTRIES);
        vault_model_t *model = conflicts != NULL ? vault_preview_snapshot(token, conflicts) : NULL;
        cJSON *reply = cJSON_CreateObject(),
              *entries = reply != NULL ? cJSON_AddArrayToObject(reply, "entries") : NULL;
        bool ok =
            model != NULL && entries != NULL && cJSON_AddStringToObject(reply, "token", token);
        for (unsigned i = 0; ok && i < model->count; i++)
        {
            cJSON *entry = vault_entry_json(&model->entries[i], true);
            if (entry == NULL || !cJSON_AddNumberToObject(entry, "conflict", conflicts[i]) ||
                !cJSON_AddItemToArray(entries, entry))
            {
                cJSON_Delete(entry);
                ok = false;
            }
        }
        vault_secure_free(model);
        vault_secure_free(conflicts);
        if (!ok)
        {
            cJSON_Delete(reply);
            reply = NULL;
        }
        return json_response(request, reply);
    }
    if (request->method != HTTP_POST)
        return error(request, "404 Not Found", "{}");
    if (strcmp(route, "/api/activity") == 0)
    {
        vault_activity();
        return error(request, "200 OK", "{}");
    }
    if (strcmp(route, "/api/lock") == 0)
    {
        vault_lock();
        return error(request, "200 OK", "{\"locked\":true}");
    }
    if (strcmp(route, "/api/import") == 0)
    {
        vault_job_t *incoming = job(VAULT_PREVIEW);
        if (incoming == NULL)
            return error(request, "503 Service Unavailable", "{}");
        char encoded[192];
        size_t length = 0;
        bool ok =
            httpd_req_get_hdr_value_str(request, "X-Vault-Import-Password", encoded,
                                        sizeof(encoded)) == ESP_OK &&
            mbedtls_base64_decode((uint8_t *)incoming->password, sizeof(incoming->password) - 1,
                                  &length, (const uint8_t *)encoded, strlen(encoded)) == 0 &&
            length > 0;
        vault_wipe(encoded, sizeof(encoded));
        if (ok && length < sizeof(incoming->password))
            incoming->password[length] = 0;
        else
            ok = false;
        if (ok)
            incoming->payload =
                body(request, VAULT_MAX_PAYLOAD + VAULT_HEADER_SIZE + VAULT_TAG_SIZE);
        incoming->size = request->content_len;
        if (!ok || incoming->payload == NULL)
        {
            vault_job_free(incoming);
            return error(request, "400 Bad Request", "{}");
        }
        return submit(request, incoming);
    }
    cJSON *json = json_body(request);
    if (!cJSON_IsObject(json))
    {
        cJSON_Delete(json);
        return error(request, "400 Bad Request", "{}");
    }
    if (strcmp(route, "/api/export") == 0)
    {
        vault_selection_t *selection = vault_secure_alloc(sizeof(*selection));
        bool ok = selection != NULL;
        if (selection != NULL)
            memset(selection, 0, sizeof(*selection));
        cJSON *ids = cJSON_GetObjectItemCaseSensitive(json, "ids");
        if (ids != NULL)
        {
            int count = cJSON_IsArray(ids) ? cJSON_GetArraySize(ids) : -1;
            ok = ok && count > 0 && count <= VAULT_ENTRIES;
            if (ok)
                selection->id_count = count;
            for (int i = 0; ok && i < count; i++)
            {
                cJSON *id = cJSON_GetArrayItem(ids, i);
                ok = cJSON_IsString(id) && strlen(id->valuestring) == 32;
                if (ok)
                    memcpy(selection->ids[i], id->valuestring, 33);
                for (int j = 0; ok && j < i; j++)
                    if (strcmp(selection->ids[i], selection->ids[j]) == 0)
                        ok = false;
            }
        }
        cJSON_Delete(json);
        size_t size = 0;
        uint8_t *file = ok ? vault_export_selection(selection, &size) : NULL;
        vault_secure_free(selection);
        if (file == NULL)
            return error(request, "400 Bad Request", "{}");
        httpd_resp_set_type(request, "application/octet-stream");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        httpd_resp_set_hdr(request, "Content-Disposition",
                           "attachment; filename=Vault-backup.wvault");
        esp_err_t result = httpd_resp_send(request, (const char *)file, size);
        vault_secure_free(file);
        vault_activity();
        return result;
    }
    if (strcmp(route, "/api/entry") == 0 || strcmp(route, "/api/save") == 0 ||
        strcmp(route, "/api/delete") == 0 || strcmp(route, "/api/otp") == 0)
    {
        cJSON *entry_json = strcmp(route, "/api/save") == 0
                                ? cJSON_GetObjectItemCaseSensitive(json, "entry")
                                : json;
        cJSON *id = cJSON_GetObjectItemCaseSensitive(entry_json, "id");
        vault_status_t status;
        if (!vault_status(&status))
        {
            cJSON_Delete(json);
            return error(request, "503 Service Unavailable", "{}");
        }
        vault_model_t *model = vault_snapshot();
        unsigned index = model != NULL ? model->count : 0;
        if (model != NULL && cJSON_IsString(id))
            for (unsigned i = 0; i < model->count; i++)
                if (strcmp(model->entries[i].id, id->valuestring) == 0)
                {
                    index = i;
                    break;
                }
        if (strcmp(route, "/api/otp") == 0)
        {
            char code[9];
            time_t now = time(NULL);
            bool ok =
                model != NULL && index < model->count && clock_is_valid() && now >= 1704067200;
            if (ok)
            {
                vault_entry_t *entry = &model->entries[index];
                ok =
                    entry->otp[0] && vault_totp(entry->otp, now, entry->otp_digits,
                                                entry->otp_period, entry->otp_hash, code) == ESP_OK;
            }
            cJSON *reply = ok ? cJSON_CreateObject() : NULL;
            if (reply != NULL &&
                (!cJSON_AddStringToObject(reply, "code", code) ||
                 !cJSON_AddNumberToObject(reply, "remaining",
                                          model->entries[index].otp_period -
                                              now % model->entries[index].otp_period)))
            {
                cJSON_Delete(reply);
                reply = NULL;
            }
            vault_wipe(code, sizeof(code));
            vault_secure_free(model);
            cJSON_Delete(json);
            return json_response(request, reply);
        }
        if (strcmp(route, "/api/entry") == 0)
        {
            cJSON *reply = model != NULL && index < model->count
                               ? vault_entry_json(&model->entries[index], true)
                               : NULL;
            char gen[24];
            snprintf(gen, sizeof(gen), "%llu", (unsigned long long)status.generation);
            if (reply != NULL && cJSON_AddStringToObject(reply, "generation", gen) == NULL)
            {
                cJSON_Delete(reply);
                reply = NULL;
            }
            vault_secure_free(model);
            cJSON_Delete(json);
            return json_response(request, reply);
        }
        vault_job_t *change = job(strcmp(route, "/api/save") == 0 ? VAULT_EDIT : VAULT_DELETE);
        bool ok = model != NULL && change != NULL && generation(json, &change->expected_generation);
        if (ok)
            change->index = index;
        if (ok && change->operation == VAULT_EDIT)
        {
            ok = vault_entry_parse(entry_json, &change->entry);
            if (ok && index == model->count)
                for (unsigned i = 0; i < model->count; i++)
                    if (strcmp(model->entries[i].id, change->entry.id) == 0)
                        ok = false;
        }
        else if (ok && index < model->count)
            change->entry = model->entries[index];
        else
            ok = false;
        vault_secure_free(model);
        cJSON_Delete(json);
        if (!ok)
        {
            vault_job_free(change);
            return error(request, "400 Bad Request", "{}");
        }
        return submit(request, change);
    }
    if (strcmp(route, "/api/resolve") == 0)
    {
        vault_job_t *change = job(VAULT_IMPORT);
        cJSON *token = cJSON_GetObjectItemCaseSensitive(json, "token"),
              *choices = cJSON_GetObjectItemCaseSensitive(json, "choices"),
              *confirmed = cJSON_GetObjectItemCaseSensitive(json, "confirmed");
        int count = cJSON_IsArray(choices) ? cJSON_GetArraySize(choices) : -1;
        char preview_token[33];
        int *conflicts = vault_secure_alloc(sizeof(int) * VAULT_ENTRIES);
        vault_model_t *preview_model =
            conflicts != NULL ? vault_preview_snapshot(preview_token, conflicts) : NULL;
        bool ok = change != NULL && preview_model != NULL && cJSON_IsString(token) &&
                  strlen(token->valuestring) == 32 &&
                  vault_equal(token->valuestring, preview_token, 33) &&
                  count == (int)preview_model->count && cJSON_IsTrue(confirmed);
        if (ok)
        {
            memcpy(change->preview, preview_token, 33);
            change->enabled = true;
            change->choices = vault_secure_alloc(sizeof(vault_resolution_t) * (count ? count : 1));
            ok = change->choices != NULL;
            for (int i = 0; ok && i < count; i++)
            {
                cJSON *choice = cJSON_GetArrayItem(choices, i);
                ok = cJSON_IsNumber(choice) && choice->valuedouble >= 0 &&
                     choice->valuedouble <= 3 && choice->valuedouble == (int)choice->valuedouble;
                if (ok)
                    change->choices[i] = (vault_resolution_t)choice->valueint;
            }
        }
        vault_secure_free(preview_model);
        vault_secure_free(conflicts);
        cJSON_Delete(json);
        if (!ok)
        {
            vault_job_free(change);
            return error(request, "400 Bad Request", "{}");
        }
        return submit(request, change);
    }
    if (strcmp(route, "/api/generate") == 0)
    {
        cJSON *length = cJSON_GetObjectItemCaseSensitive(json, "length");
        char password[129];
        bool valid = cJSON_IsNumber(length) && length->valuedouble >= 12 &&
                     length->valuedouble <= 128 && length->valuedouble == length->valueint;
        cJSON *reply = cJSON_CreateObject();
        if (!valid || vault_generate(password, (unsigned)length->valueint, true) != ESP_OK ||
            reply == NULL || cJSON_AddStringToObject(reply, "password", password) == NULL)
        {
            cJSON_Delete(reply);
            reply = NULL;
        }
        vault_wipe(password, sizeof(password));
        cJSON_Delete(json);
        vault_activity();
        return json_response(request, reply);
    }
    if (strcmp(route, "/api/preview/edit") == 0)
    {
        vault_job_t *change = job(VAULT_PREVIEW_EDIT);
        cJSON *index = cJSON_GetObjectItemCaseSensitive(json, "index"),
              *token = cJSON_GetObjectItemCaseSensitive(json, "token"),
              *entry = cJSON_GetObjectItemCaseSensitive(json, "entry");
        bool ok = change != NULL && cJSON_IsNumber(index) && index->valuedouble >= 0 &&
                  index->valuedouble < VAULT_ENTRIES && index->valuedouble == index->valueint &&
                  cJSON_IsString(token) && strlen(token->valuestring) == 32 &&
                  vault_entry_parse(entry, &change->entry);
        if (ok)
        {
            change->index = index->valueint;
            memcpy(change->preview, token->valuestring, 33);
        }
        cJSON_Delete(json);
        if (!ok)
        {
            vault_job_free(change);
            return error(request, "400 Bad Request", "{}");
        }
        return submit(request, change);
    }
    cJSON_Delete(json);
    return error(request, "404 Not Found", "{}");
}

static esp_err_t certificate(const char *ip)
{
    s_certificate = vault_secure_alloc(2048);
    s_private = vault_secure_alloc(1024);
    if (s_certificate == NULL || s_private == NULL)
        return ESP_ERR_NO_MEM;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attributes, MBEDTLS_PK_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_key_id_t key = 0;
    int result = psa_generate_key(&attributes, &key) == PSA_SUCCESS ? 0 : -1;
    psa_reset_key_attributes(&attributes);
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    uint8_t serial[16] = {0}, san[8] = {0x30, 6, 0x87, 4, 0, 0, 0, 0};
    if (result == 0 && vault_random(serial, 16) != ESP_OK)
        result = -1;
    serial[0] &= 127;
    serial[0] |= 1;
    if (result == 0)
        result = mbedtls_pk_wrap_psa(&pk, key);
    if (result == 0)
        result = mbedtls_pk_write_key_pem(&pk, s_private, 1024);
    mbedtls_x509write_crt_set_version(&crt, 2);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    if (result == 0)
        result = mbedtls_x509write_crt_set_subject_name(&crt, "CN=Watch Vault");
    if (result == 0)
        result = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=Watch Vault");
    if (result == 0)
        result = mbedtls_x509write_crt_set_serial_raw(&crt, serial, 16);
    if (result == 0)
        result = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20350101000000");
    if (result == 0)
        result = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    struct in_addr address = {0};
    if (inet_pton(AF_INET, ip, &address) != 1)
        result = -1;
    memcpy(san + 4, &address, 4);
    if (result == 0)
        result = mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_ALT_NAME,
                                                     MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME),
                                                     0, san, sizeof(san));
    if (result == 0)
        result = mbedtls_x509write_crt_pem(&crt, s_certificate, 2048);
    uint8_t digest[32];
    size_t size = 0;
    mbedtls_x509_crt parsed;
    mbedtls_x509_crt_init(&parsed);
    if (result == 0)
        result =
            mbedtls_x509_crt_parse(&parsed, s_certificate, strlen((const char *)s_certificate) + 1);
    if (result == 0 && (psa_hash_compute(PSA_ALG_SHA_256, parsed.raw.p, parsed.raw.len, digest,
                                         sizeof(digest), &size) != PSA_SUCCESS ||
                        size != 32))
        result = -1;
    if (result == 0)
    {
        char fingerprint[96];
        for (unsigned i = 0; i < 32; i++)
            snprintf(fingerprint + i * 3, 4, i == 31 ? "%02X" : "%02X:", digest[i]);
        portENTER_CRITICAL(&s_info);
        memcpy(s_fingerprint, fingerprint, sizeof(s_fingerprint));
        portEXIT_CRITICAL(&s_info);
    }
    mbedtls_x509_crt_free(&parsed);
    vault_wipe(serial, sizeof(serial));
    vault_wipe(digest, sizeof(digest));
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    if (key)
        psa_destroy_key(key);
#if VAULT_QA
    if (result != 0)
        printf("@VAULTCERT error %d (-0x%04x)\n", result, (unsigned)-result);
#endif
    return result == 0 ? ESP_OK : ESP_FAIL;
}

void vault_web_stop(void)
{
    portENTER_CRITICAL(&s_info);
    vault_wipe(s_token, sizeof(s_token));
    vault_wipe(s_code, sizeof(s_code));
    s_url[0] = s_host[0] = s_fingerprint[0] = 0;
    portEXIT_CRITICAL(&s_info);
    if (s_server != NULL)
    {
        httpd_ssl_stop(s_server);
        s_server = NULL;
        power_keep_screen_on(false);
    }
    vault_secure_free(s_certificate);
    s_certificate = NULL;
    vault_secure_free(s_private);
    s_private = NULL;
    if (s_wifi)
    {
        wifi_release();
        s_wifi = false;
    }
}

esp_err_t vault_web_start(void)
{
    if (s_server != NULL)
        return ESP_OK;
    if (!vault_unlocked())
        return ESP_ERR_INVALID_STATE;
    esp_err_t result = wifi_acquire(20000);
    if (result != ESP_OK)
    {
        QA_FAIL("wifi", result);
        return result;
    }
    s_wifi = true;
    char ip[16];
    if (!wifi_get_ip(ip, sizeof(ip)))
    {
        QA_FAIL("ip", ESP_FAIL);
        vault_web_stop();
        return ESP_FAIL;
    }
    result = certificate(ip);
    if (result != ESP_OK)
    {
        QA_FAIL("certificate", result);
        vault_web_stop();
        return result;
    }
    uint32_t random;
    do
    {
        result = vault_random(&random, sizeof(random));
    } while (result == ESP_OK && random >= UINT32_MAX - (UINT32_MAX % 100000000U));
    if (result != ESP_OK)
    {
        vault_web_stop();
        return result;
    }
    portENTER_CRITICAL(&s_info);
    snprintf(s_url, sizeof(s_url), "https://%s", ip);
    snprintf(s_host, sizeof(s_host), "%s", ip);
    snprintf(s_code, sizeof(s_code), "%08lu", (unsigned long)(random % 100000000U));
    portEXIT_CRITICAL(&s_info);
    s_attempts = 0;
    s_pair_window = esp_timer_get_time();
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.httpd.max_uri_handlers = 20;
    config.httpd.max_open_sockets = 2;
    config.httpd.lru_purge_enable = true;
    config.httpd.stack_size = 16384;
    /* Internal RAM is scarce with BLE + USB + Wi-Fi: the stack lives in PSRAM like the vault data. */
    config.httpd.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    config.httpd.recv_wait_timeout = 3;
    config.httpd.send_wait_timeout = 3;
    config.tls_handshake_timeout_ms = 5000;
    config.servercert = s_certificate;
    config.servercert_len = strlen((const char *)s_certificate) + 1;
    config.prvtkey_pem = s_private;
    config.prvtkey_len = strlen((const char *)s_private) + 1;
    result = httpd_ssl_start(&s_server, &config);
    if (result != ESP_OK)
    {
        QA_FAIL("https", result);
        vault_web_stop();
        return result;
    }
    power_keep_screen_on(true);
    const httpd_uri_t routes[] = {{.uri = "/", .method = HTTP_GET, .handler = asset},
                                  {.uri = "/dashboard.js", .method = HTTP_GET, .handler = asset},
                                  {.uri = "/api/pair", .method = HTTP_POST, .handler = pair},
                                  {.uri = "/api/status", .method = HTTP_GET, .handler = api},
                                  {.uri = "/api/entries", .method = HTTP_GET, .handler = api},
                                  {.uri = "/api/preview", .method = HTTP_GET, .handler = api},
                                  {.uri = "/api/entry", .method = HTTP_POST, .handler = api},
                                  {.uri = "/api/save", .method = HTTP_POST, .handler = api},
                                  {.uri = "/api/delete", .method = HTTP_POST, .handler = api},
                                  {.uri = "/api/export", .method = HTTP_POST, .handler = api},
                                  {.uri = "/api/import", .method = HTTP_POST, .handler = api},
                                  {.uri = "/api/resolve", .method = HTTP_POST, .handler = api},
                                  {.uri = "/api/preview/edit", .method = HTTP_POST, .handler = api},
                                  {.uri = "/api/generate", .method = HTTP_POST, .handler = api},
                                  {.uri = "/api/lock", .method = HTTP_POST, .handler = api},
                                  {.uri = "/api/otp", .method = HTTP_POST, .handler = api},
                                  {.uri = "/api/activity", .method = HTTP_POST, .handler = api}};
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        if ((result = httpd_register_uri_handler(s_server, &routes[i])) != ESP_OK)
        {
            vault_web_stop();
            return result;
        }
#if VAULT_QA
    printf("@VAULTWEB %s code=%s fingerprint=%s\n", s_url, s_code, s_fingerprint);
    fflush(stdout);
#endif
    return ESP_OK;
}
