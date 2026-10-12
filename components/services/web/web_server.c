#include "services/web_server.h"

#include "core/clock.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "hardware/board.h"
#include "hardware/pmu.h"
#include "hardware/sdcard.h"
#include "services/storage.h"
#include "services/extapp.h"
#include "services/alarm.h"
#include "services/fw_update.h"
#include "services/wifi.h"

#include "captive_dns.h"
#include "web_files.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "mdns.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "web";

#define BODY_MAX    (32 * 1024)

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server = NULL;
static bool s_ap_mode = false;
static char s_ap_name[33] = "";
static char s_error[64] = "";

/* "Salva e collega" from the page: runs in its own task, the page polls the result. */
typedef enum
{
    CONNECT_IDLE,
    CONNECT_RUNNING,
    CONNECT_OK,
    CONNECT_FAILED,
} connect_state_t;

typedef struct
{
    char ssid[33];
    char password[65];
} connect_job_t;

static volatile connect_state_t s_connect = CONNECT_IDLE;
static char s_connect_ssid[33] = "";
static bool s_connect_holds_wifi = false;   /* a successful connect holds a Wi-Fi reference */

/* ------------------------------------------------------------ helpers */

/* Read the whole request body (small JSON / text). Caller frees. */
static char *read_body(httpd_req_t *req)
{
    if (req->content_len >= BODY_MAX)
    {
        return NULL;
    }

    char *body = malloc(req->content_len + 1);

    if (body == NULL)
    {
        return NULL;
    }

    size_t got = 0;

    while (got < req->content_len)
    {
        int n = httpd_req_recv(req, body + got, req->content_len - got);

        if (n <= 0)
        {
            free(body);
            return NULL;
        }

        got += (size_t)n;
    }

    body[got] = '\0';

    return body;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text != NULL ? text : "{}");
    cJSON_free(text);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req, bool ok, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", ok);

    if (message != NULL)
    {
        cJSON_AddStringToObject(json, "message", message);
    }

    return send_json(req, json);
}

static cJSON *parse_body(httpd_req_t *req)
{
    char *body = read_body(req);
    cJSON *json = body != NULL ? cJSON_Parse(body) : NULL;
    free(body);
    return json;
}

static bool get_bool(const cJSON *json, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItem(json, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static int get_int(const cJSON *json, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItem(json, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static void get_string(const cJSON *json, const char *key, char *out, size_t size)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(json, key));

    if (value != NULL)
    {
        snprintf(out, size, "%s", value);
    }
}

/* ------------------------------------------------------------ handlers */

static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t handle_state(httpd_req_t *req)
{
    const settings_t *s = settings_get();
    cJSON *json = cJSON_CreateObject();
    char text[40];

    cJSON_AddStringToObject(json, "product", board_info()->product);
    cJSON_AddNumberToObject(json, "battery", pmu_battery_percent());
    cJSON_AddBoolToObject(json, "charging", pmu_is_charging());
    wifi_current_ssid(text, sizeof(text));
    cJSON_AddStringToObject(json, "wifi", text);
    cJSON_AddNumberToObject(json, "time", (double)time(NULL));
    cJSON_AddBoolToObject(json, "sd", sdcard_is_mounted());
    cJSON_AddBoolToObject(json, "has_sdcard", board_info()->has_sdcard);
    cJSON_AddBoolToObject(json, "files", storage_dir() != NULL);
    cJSON_AddStringToObject(json, "version", fw_update_running_version());

    cJSON *set = cJSON_AddObjectToObject(json, "settings");
    cJSON_AddNumberToObject(set, "brightness", s->brightness);
    cJSON_AddNumberToObject(set, "screen_timeout", s->screen_timeout_s);
    cJSON_AddBoolToObject(set, "always_on", s->always_on);
    cJSON_AddBoolToObject(set, "tap_to_wake", s->tap_to_wake);
    cJSON_AddBoolToObject(set, "wake_on_notification", s->wake_on_notification);
    cJSON_AddNumberToObject(set, "volume", s->volume);
    cJSON_AddBoolToObject(set, "time_24h", s->time_24h);
    cJSON_AddStringToObject(set, "timezone", s->timezone);
    cJSON_AddBoolToObject(set, "auto_time", s->auto_time);
    cJSON_AddBoolToObject(set, "ble", s->ble_enabled);
    cJSON_AddStringToObject(set, "device_name", s->device_name);
    cJSON_AddStringToObject(set, "weather_city", s->weather_city);
    cJSON_AddNumberToObject(set, "watchface", s->watchface);
    cJSON_AddStringToObject(set, "update_url", s->update_url);

    return send_json(req, json);
}

static esp_err_t handle_settings(httpd_req_t *req)
{
    cJSON *json = parse_body(req);

    if (json == NULL)
    {
        return send_ok(req, false, "JSON non valido");
    }

    settings_t s = *settings_get();

    s.brightness = (uint8_t)get_int(json, "brightness", s.brightness);
    s.screen_timeout_s = (uint8_t)get_int(json, "screen_timeout", s.screen_timeout_s);
    s.always_on = get_bool(json, "always_on", s.always_on);
    s.tap_to_wake = get_bool(json, "tap_to_wake", s.tap_to_wake);
    s.wake_on_notification = get_bool(json, "wake_on_notification", s.wake_on_notification);
    s.volume = (uint8_t)get_int(json, "volume", s.volume);
    s.time_24h = get_bool(json, "time_24h", s.time_24h);
    s.auto_time = get_bool(json, "auto_time", s.auto_time);
    s.ble_enabled = get_bool(json, "ble", s.ble_enabled);
    s.watchface = (uint8_t)get_int(json, "watchface", s.watchface);
    get_string(json, "timezone", s.timezone, sizeof(s.timezone));
    get_string(json, "device_name", s.device_name, sizeof(s.device_name));
    get_string(json, "update_url", s.update_url, sizeof(s.update_url));
    cJSON_Delete(json);

    settings_save(&s);
    clock_set_timezone(s.timezone);
    power_settings_changed();

    return send_ok(req, true, NULL);
}

static esp_err_t handle_time(httpd_req_t *req)
{
    cJSON *json = parse_body(req);
    double epoch = cJSON_GetNumberValue(cJSON_GetObjectItem(json, "epoch"));
    cJSON_Delete(json);

    if (epoch < 1700000000.0)
    {
        return send_ok(req, false, "Ora non valida");
    }

    clock_set_utc((time_t)epoch);

    return send_ok(req, true, NULL);
}

static esp_err_t handle_wifi_list(httpd_req_t *req)
{
    wifi_known_t *known = calloc(WIFI_KNOWN_MAX, sizeof(wifi_known_t));
    cJSON *json = cJSON_CreateArray();
    int count = known != NULL ? wifi_known_list(known, WIFI_KNOWN_MAX) : 0;

    for (int i = 0; i < count; i++)
    {
        cJSON_AddItemToArray(json, cJSON_CreateString(known[i].ssid));   /* never the password */
    }

    free(known);

    return send_json(req, json);
}

static void connect_task(void *arg)
{
    connect_job_t *job = arg;

    /* With our access point on, the radio moves to the router's channel: phones reconnect by themselves. */
    esp_err_t err = wifi_connect_new(job->ssid, job->password, 20000);

    if (err == ESP_OK && !s_connect_holds_wifi)
    {
        s_connect_holds_wifi = true;   /* released by web_server_stop() */
    }
    else if (err == ESP_OK)
    {
        wifi_release();   /* already holding one */
    }

    ESP_LOGI(TAG, "Connect \"%s\": %s", job->ssid, err == ESP_OK ? "ok" : wifi_reason_text(wifi_last_reason()));
    s_connect = err == ESP_OK ? CONNECT_OK : CONNECT_FAILED;
    free(job);
    vTaskDelete(NULL);
}

static esp_err_t handle_wifi_add(httpd_req_t *req)
{
    cJSON *json = parse_body(req);
    char ssid[33] = "";
    char password[65] = "";
    get_string(json, "ssid", ssid, sizeof(ssid));
    get_string(json, "password", password, sizeof(password));
    bool remove = get_bool(json, "delete", false);
    bool connect = get_bool(json, "connect", false);
    cJSON_Delete(json);

    if (ssid[0] == '\0')
    {
        return send_ok(req, false, "Nome rete mancante");
    }

    esp_err_t err = remove ? wifi_known_remove(ssid) : wifi_known_add(ssid, password);

    if (err != ESP_OK || remove || !connect)
    {
        return send_ok(req, err == ESP_OK, err == ESP_OK ? NULL : "Salvataggio rete non riuscito");
    }

    if (s_connect == CONNECT_RUNNING)
    {
        return send_ok(req, false, "Collegamento già in corso");
    }

    connect_job_t *job = malloc(sizeof(connect_job_t));

    if (job == NULL)
    {
        return send_ok(req, false, "Memoria insufficiente");
    }

    snprintf(job->ssid, sizeof(job->ssid), "%s", ssid);
    snprintf(job->password, sizeof(job->password), "%s", password);
    snprintf(s_connect_ssid, sizeof(s_connect_ssid), "%s", ssid);
    s_connect = CONNECT_RUNNING;

    if (!sys_task_create(connect_task, "web_connect", 4096, job, 3, SYS_CORE_ANY))
    {
        free(job);
        s_connect = CONNECT_FAILED;
        return send_ok(req, false, "Memoria insufficiente");
    }

    return send_ok(req, true, "Salvata: collegamento in corso");
}

static esp_err_t handle_wifi_status(httpd_req_t *req)
{
    static const char *const names[] = {"idle", "connecting", "ok", "failed"};
    char ip[16] = "";
    cJSON *json = cJSON_CreateObject();

    if (wifi_is_connected())
    {
        wifi_get_ip(ip, sizeof(ip));
    }

    cJSON_AddStringToObject(json, "state", names[s_connect]);
    cJSON_AddStringToObject(json, "ssid", s_connect_ssid);
    cJSON_AddStringToObject(json, "reason", wifi_reason_text(wifi_last_reason()));
    cJSON_AddStringToObject(json, "ip", ip);

    return send_json(req, json);
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    wifi_ap_t *list = calloc(WIFI_SCAN_MAX, sizeof(wifi_ap_t));
    int count = list != NULL ? wifi_scan(list, WIFI_SCAN_MAX) : 0;
    cJSON *json = cJSON_CreateArray();

    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", list[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", list[i].rssi);
        cJSON_AddBoolToObject(item, "open", list[i].open);
        cJSON_AddBoolToObject(item, "known", list[i].known);
        cJSON_AddItemToArray(json, item);
    }

    free(list);

    return send_json(req, json);
}

static esp_err_t handle_alarms(httpd_req_t *req)
{
    alarm_t *alarms = calloc(ALARM_MAX, sizeof(alarm_t));
    cJSON *json = cJSON_CreateArray();
    int count = alarms != NULL ? alarm_list(alarms, ALARM_MAX) : 0;

    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", alarms[i].id);
        cJSON_AddNumberToObject(item, "hour", alarms[i].hour);
        cJSON_AddNumberToObject(item, "minute", alarms[i].minute);
        cJSON_AddNumberToObject(item, "days", alarms[i].days);
        cJSON_AddBoolToObject(item, "enabled", alarms[i].enabled);
        cJSON_AddNumberToObject(item, "radio", alarms[i].radio);
        cJSON_AddStringToObject(item, "label", alarms[i].label);
        cJSON_AddItemToArray(json, item);
    }

    free(alarms);

    return send_json(req, json);
}

static esp_err_t handle_alarm_save(httpd_req_t *req)
{
    cJSON *json = parse_body(req);

    if (json == NULL)
    {
        return send_ok(req, false, "JSON non valido");
    }

    uint32_t id = (uint32_t)get_int(json, "id", 0);
    esp_err_t err;

    if (get_bool(json, "delete", false))
    {
        err = alarm_delete(id);
    }
    else
    {
        alarm_t a = {.id = id, .radio = -1, .enabled = true};
        alarm_get(id, &a);
        a.hour = (uint8_t)get_int(json, "hour", a.hour);
        a.minute = (uint8_t)get_int(json, "minute", a.minute);
        a.days = (uint8_t)get_int(json, "days", a.days);
        a.enabled = get_bool(json, "enabled", a.enabled);
        a.radio = (int16_t)get_int(json, "radio", a.radio);
        get_string(json, "label", a.label, sizeof(a.label));
        err = alarm_save(&a);
    }

    cJSON_Delete(json);

    return send_ok(req, err == ESP_OK, err == ESP_OK ? NULL : esp_err_to_name(err));
}

/* Raw .bin upload: POST /api/upload?name=MyApp -> /sdcard/apps/MyApp/MyApp.bin */
static esp_err_t handle_upload_exclusive(httpd_req_t *req);

static esp_err_t handle_upload(httpd_req_t *req)
{
    if (!storage_files_acquire()) return send_ok(req, false, "Memoria file occupata");
    esp_err_t result = handle_upload_exclusive(req);
    storage_files_release();
    return result;
}

static esp_err_t handle_upload_exclusive(httpd_req_t *req)
{
    char query[64] = "";
    char name[32] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || name[0] == '\0' ||
        strchr(name, '/') != NULL || strchr(name, '.') != NULL)
    {
        return send_ok(req, false, "Nome app non valido");
    }

    const char *root = storage_dir();
    if (root == NULL)
    {
        return send_ok(req, false, "Memoria file non disponibile");
    }
    for (const char *p = name; *p != '\0'; p++)
    {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
        {
            return send_ok(req, false, "Nome app non valido");
        }
    }
    if (req->content_len == 0)
    {
        return send_ok(req, false, "Nome app non valido");
    }
    if (!sdcard_is_mounted())
    {
        size_t total_kb, free_kb;
        storage_flash_space(&total_kb, &free_kb);
        if (req->content_len <= 0 || (size_t)req->content_len > free_kb * 1024)
        {
            return send_ok(req, false, "Spazio insufficiente: conserva l'app attuale o rimuovila dalla pagina File prima di aggiornare");
        }
    }
    char path[96], final_path[96];
    snprintf(path, sizeof(path), "%s/apps", root);
    mkdir(path, 0775);
    snprintf(path, sizeof(path), "%s/apps/%s", root, name);
    mkdir(path, 0775);
    snprintf(final_path, sizeof(final_path), "%s/apps/%s/%s.bin", root, name, name);
    snprintf(path, sizeof(path), "%s/apps/%s/.upload", root, name);

    FILE *file = fopen(path, "wb");

    if (file == NULL)
    {
        return send_ok(req, false, "Scrittura file fallita");
    }

    char *buffer = malloc(8192);
    size_t left = req->content_len;
    bool ok = buffer != NULL;

    power_keep_screen_on(true);

    while (ok && left > 0)
    {
        int n = httpd_req_recv(req, buffer, left < 8192 ? left : 8192);

        if (n == HTTPD_SOCK_ERR_TIMEOUT)
        {
            continue;
        }

        ok = n > 0 && fwrite(buffer, 1, (size_t)n, file) == (size_t)n;
        left -= n > 0 ? (size_t)n : 0;
    }

    power_keep_screen_on(false);
    free(buffer);
    ok = fflush(file) == 0 && fsync(fileno(file)) == 0 && ok;
    ok = fclose(file) == 0 && ok;
    if (ok)
    {
        /* FatFS rename cannot overwrite. Keep the old image until publication
         * succeeds; extapp_scan restores it after an interrupted replacement. */
        char previous[96];
        snprintf(previous, sizeof(previous), "%s/apps/%s/.previous", root, name);
        struct stat st;
        bool existed = stat(final_path, &st) == 0;
        if (existed)
        {
            remove(previous);
            ok = rename(final_path, previous) == 0;
        }
        if (ok)
        {
            ok = rename(path, final_path) == 0;
            if (!ok && existed)
            {
                rename(previous, final_path);
            }
        }
        if (ok)
        {
            char folder[96], leaf[40];
            snprintf(folder, sizeof(folder), "%s/apps/%s", root, name);
            snprintf(leaf, sizeof(leaf), "%s.bin", name);
            ok = extapp_upload_manifest(folder, leaf) == ESP_OK;
            if (!ok)
            {
                remove(final_path);
                if (existed) rename(previous, final_path);
            }
        }
        if (ok)
        {
            remove(previous);
            extapp_forget_cache();
        }
    }
    if (!ok)
    {
        remove(path);
    }
    ESP_LOGI(TAG, "Upload %s: %s", final_path, ok ? "ok" : "failed");

    return send_ok(req, ok, ok ? "App caricata" : "Upload interrotto");
}

/*
 * Firmware update: POST /api/firmware with the raw build/<project>.bin.
 * Streamed straight into the other app slot (8 KB at a time, no copy on the
 * SD card); the watch shows the progress and restarts when it is verified.
 */
static esp_err_t handle_firmware(httpd_req_t *req)
{
    if (fw_update_begin(req->content_len) != ESP_OK)
    {
        const char *why = fw_update_message();
        return send_ok(req, false, why[0] != '\0' ? why : "Aggiornamento già in corso");
    }

    char *buffer = malloc(8192);
    size_t left = req->content_len;
    esp_err_t err = buffer != NULL ? ESP_OK : ESP_ERR_NO_MEM;

    if (buffer == NULL)
    {
        fw_update_abort("Memoria insufficiente");
    }

    while (err == ESP_OK && left > 0)
    {
        int n = httpd_req_recv(req, buffer, left < 8192 ? left : 8192);

        if (n == HTTPD_SOCK_ERR_TIMEOUT)
        {
            continue;
        }

        if (n <= 0)
        {
            fw_update_abort("Upload interrotto");
            err = ESP_FAIL;
            break;
        }

        err = fw_update_write(buffer, (size_t)n);   /* aborts by itself on error */
        left -= (size_t)n;
    }

    free(buffer);

    if (err == ESP_OK)
    {
        err = fw_update_end();
    }

    return send_ok(req, err == ESP_OK, err == ESP_OK ? "Firmware installato: riavvio in corso" : fw_update_message());
}

/*
 * Own access point only: any unknown address (the phones' "is there
 * internet?" checks: /generate_204, /hotspot-detect.html, /connecttest.txt...)
 * is sent to the setup page. The phone then shows "sign in to network" and
 * keeps the connection.
 */
static esp_err_t handle_not_found(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;

    if (!s_ap_mode)
    {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }

    char location[40];
    char ip[16] = "192.168.4.1";
    wifi_get_ip(ip, sizeof(ip));
    snprintf(location, sizeof(location), "http://%s/", ip);

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------- server */

static esp_err_t start_httpd(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.stack_size = 6144;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 15;

    esp_err_t err = httpd_start(&s_server, &config);

    if (err != ESP_OK)
    {
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handle_index},
        {.uri = "/api/state", .method = HTTP_GET, .handler = handle_state},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = handle_settings},
        {.uri = "/api/time", .method = HTTP_POST, .handler = handle_time},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = handle_wifi_list},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = handle_wifi_add},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan},
        {.uri = "/api/wifi/status", .method = HTTP_GET, .handler = handle_wifi_status},
        {.uri = "/api/alarms", .method = HTTP_GET, .handler = handle_alarms},
        {.uri = "/api/alarms", .method = HTTP_POST, .handler = handle_alarm_save},
        {.uri = "/api/upload", .method = HTTP_POST, .handler = handle_upload},
        {.uri = "/api/firmware", .method = HTTP_POST, .handler = handle_firmware},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
    {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    web_files_register(s_server);   /* SD card file manager */
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_not_found);

    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (s_server != NULL)
    {
        return ESP_OK;
    }

    s_error[0] = '\0';
    s_ap_mode = wifi_acquire(15000) != ESP_OK;

    if (s_ap_mode)
    {
        wifi_start_access_point(s_ap_name, sizeof(s_ap_name));

        char ip[16] = "192.168.4.1";
        wifi_get_ip(ip, sizeof(ip));

        if (captive_dns_start(inet_addr(ip)) != ESP_OK)
        {
            ESP_LOGW(TAG, "No captive DNS: open http://%s by hand", ip);
        }
    }

    esp_err_t err = start_httpd();

    if (err != ESP_OK)
    {
        snprintf(s_error, sizeof(s_error), "Server non avviato: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_error);
        web_server_stop();
        return err;
    }

    if (mdns_init() == ESP_OK)
    {
        mdns_hostname_set("watch");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    ESP_LOGI(TAG, "Web page running (%s)", s_ap_mode ? s_ap_name : "Wi-Fi");

    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server != NULL)
    {
        httpd_stop(s_server);
        s_server = NULL;
        mdns_free();
    }

    if (s_connect_holds_wifi)
    {
        s_connect_holds_wifi = false;
        wifi_release();
    }

    s_connect = CONNECT_IDLE;

    if (s_ap_mode)
    {
        captive_dns_stop();
        wifi_stop_access_point();
        s_ap_mode = false;
    }
    else
    {
        wifi_release();
    }
}

const char *web_server_last_error(void)
{
    return s_error[0] != '\0' ? s_error : "Errore sconosciuto";
}

bool web_server_running(void)
{
    return s_server != NULL;
}

void web_server_url(char *out, size_t size)
{
    char ip[20];

    if (wifi_get_ip(ip, sizeof(ip)))
    {
        snprintf(out, size, "http://%s/", ip);
    }
    else
    {
        snprintf(out, size, "http://192.168.4.1/");
    }
}

bool web_server_ap_name(char *out, size_t size)
{
    snprintf(out, size, "%s", s_ap_mode ? s_ap_name : "");
    return s_ap_mode;
}
