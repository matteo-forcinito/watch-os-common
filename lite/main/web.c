#include "core/clock.h"
#include "core/settings.h"
#include "core/sys.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "giornata.h"
#include "hardware/board.h"
#include "hardware/pmu.h"
#include "life_store.h"
#include "lite.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char page_start[] asm("_binary_dashboard_html_start"), page_end[] asm("_binary_dashboard_html_end");
extern const char kit_js_start[] asm("_binary_kit_js_start"), kit_js_end[] asm("_binary_kit_js_end");
extern const char kit_css_start[] asm("_binary_kit_css_start"), kit_css_end[] asm("_binary_kit_css_end");
extern const char waste_start[] asm("_binary_raccolta_dashboard_html_start"),
    waste_end[] asm("_binary_raccolta_dashboard_html_end");
extern const char dogs_start[] asm("_binary_zampe_dashboard_html_start"), dogs_end[] asm(
                                                                              "_binary_zampe_dashboard_html_end");
static httpd_handle_t server;
static esp_netif_t *netif;
static atomic_bool running;
static atomic_llong used_at, retry_at;
static char code[7], token[33], address[64];
static int pair_failures;
static int64_t pair_window;
static esp_event_handler_instance_t wifi_handler, ip_handler;
static bool handlers;
static esp_err_t json(httpd_req_t *r, cJSON *j)
{
    if (!j)
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Memoria insufficiente");
    char *raw = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!raw)
        return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(r, raw);
    free(raw);
    return err;
}
static esp_err_t bad(httpd_req_t *r, const char *text)
{
    return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, text);
}
static cJSON *body(httpd_req_t *r)
{
    if (r->content_len <= 0 || r->content_len > 4096)
        return NULL;
    char *raw = malloc((size_t)r->content_len + 1);
    if (!raw)
        return NULL;
    int n = 0;
    while (n < r->content_len)
    {
        int got = httpd_req_recv(r, raw + n, (size_t)(r->content_len - n));
        if (got <= 0)
        {
            free(raw);
            return NULL;
        }
        n += got;
    }
    raw[n] = 0;
    cJSON *j = cJSON_Parse(raw);
    free(raw);
    return j;
}
static bool origin(httpd_req_t *r)
{
    if (!httpd_req_get_hdr_value_len(r, "Origin"))
        return true;
    char o[100], h[80], expected[100];
    if (httpd_req_get_hdr_value_str(r, "Origin", o, sizeof(o)) != ESP_OK ||
        httpd_req_get_hdr_value_str(r, "Host", h, sizeof(h)) != ESP_OK)
        return false;
    snprintf(expected, sizeof(expected), "http://%s", h);
    return !strcmp(o, expected);
}
static bool authorized(httpd_req_t *r)
{
    char k[40];
    if (!origin(r) || httpd_req_get_hdr_value_str(r, "X-Key", k, sizeof(k)) != ESP_OK || strlen(k) != 32)
        return false;
    unsigned diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= (unsigned char)k[i] ^ (unsigned char)token[i];
    if (diff)
        return false;
    atomic_store(&used_at, esp_timer_get_time());
    return true;
}
static esp_err_t denied(httpd_req_t *r)
{
    httpd_resp_set_status(r, "401 Unauthorized");
    return httpd_resp_sendstr(r, "Abbina il browser con il codice sul dispositivo");
}
static esp_err_t asset(httpd_req_t *r, const char *type, const char *start, const char *end)
{
    httpd_resp_set_type(r, type);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, start, end - start - 1);
}
static esp_err_t state(httpd_req_t *r)
{
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "internal", (double)sys_internal_free());
    cJSON_AddNumberToObject(out, "largest", (double)sys_internal_largest());
    cJSON_AddNumberToObject(out, "psram", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(out, "battery", pmu_battery_percent());
    cJSON_AddBoolToObject(out, "aod", lite_settings.aod);
    cJSON_AddNumberToObject(out, "brightness", lite_settings.brightness);
    cJSON_AddNumberToObject(out, "web_minutes", lite_settings.web_minutes);
    cJSON_AddNumberToObject(out, "utc", (double)time(NULL));
    cJSON_AddStringToObject(out, "timezone", settings_get()->timezone);
    cJSON_AddBoolToObject(out, "lvgl", false);
    cJSON_AddBoolToObject(out, "ringing",lite_ringing());
    cJSON_AddNumberToObject(out,"ui_frames",lite_draw_frames());
    cJSON_AddBoolToObject(out, "ble", false);
    int ui_page; bool ui_idle; lite_ui_snapshot(&ui_page, &ui_idle);
    cJSON_AddNumberToObject(out, "ui_page", ui_page);
    cJSON_AddBoolToObject(out, "ui_idle", ui_idle);
    cJSON *alarms = cJSON_AddArrayToObject(out, "alarms");
    alarm_t list[ALARM_MAX];
    int count = alarm_list(list, ALARM_MAX);
    for (int i = 0; i < count; i++)
    {
        alarm_t *a = &list[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", a->id);
        cJSON_AddNumberToObject(o, "hour", a->hour);
        cJSON_AddNumberToObject(o, "minute", a->minute);
        cJSON_AddNumberToObject(o, "days", a->days);
        cJSON_AddBoolToObject(o, "enabled", a->enabled);
        cJSON_AddStringToObject(o, "label", a->label);
        cJSON_AddItemToArray(alarms, o);
    }
    cJSON *items = cJSON_AddArrayToObject(out, "events");
    gd_lock();
    int n;
    const gd_item_t *events = gd_items(&n);
    ics_time_t now = gd_now();
    for (int i = 0, shown = 0; i < n && shown < 40; i++)
        if (events[i].end >= now)
        {
            const gd_item_t *v = &events[i];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "title", v->title);
            cJSON_AddNumberToObject(o, "start", (double)v->start);
            cJSON_AddNumberToObject(o, "end", (double)v->end);
            cJSON_AddItemToArray(items, o);
            shown++;
        }
    gd_unlock();
    gd_status_t status;
    gd_status(&status);
    cJSON_AddNumberToObject(out, "calendar_synced", (double)status.synced);
    cJSON *tasks = cJSON_AddArrayToObject(out, "tasks");
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 8;
    if (cap <= 80)
    {
        TaskStatus_t *snapshot = heap_caps_calloc(cap, sizeof(*snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        char (*names)[configMAX_TASK_NAME_LEN] = heap_caps_calloc(cap,configMAX_TASK_NAME_LEN,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (snapshot && names)
        {
            vTaskSuspendAll();
            UBaseType_t total = uxTaskGetSystemState(snapshot, cap, NULL);
            for (UBaseType_t i=0;i<total;i++) snprintf(names[i],configMAX_TASK_NAME_LEN,"%s",snapshot[i].pcTaskName);
            xTaskResumeAll();
            for (UBaseType_t i = 0; i < total; i++)
            {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "name", names[i]);
                cJSON_AddNumberToObject(o, "stack_free_min", snapshot[i].usStackHighWaterMark);
                cJSON_AddItemToArray(tasks, o);
            }
        }
        free(snapshot);free(names);
    }
    return json(r, out);
}
static esp_err_t get(httpd_req_t *r)
{
    const char *path = r->uri;
    if (!strcmp(path, "/"))
        return asset(r, "text/html; charset=utf-8", page_start, page_end);
    if (!strcmp(path, "/kit.js"))
        return asset(r, "application/javascript", kit_js_start, kit_js_end);
    if (!strcmp(path, "/kit.css"))
        return asset(r, "text/css", kit_css_start, kit_css_end);
    if (!strcmp(path, "/raccolta/"))
        return asset(r, "text/html; charset=utf-8", waste_start, waste_end);
    if (!strcmp(path, "/zampe/"))
        return asset(r, "text/html; charset=utf-8", dogs_start, dogs_end);
    if (!authorized(r))
        return denied(r);
    if (strstr(path, "/api/kit"))
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "app", "Watch Lite");
        cJSON_AddStringToObject(o, "board", board_info()->name);
        cJSON_AddNumberToObject(o, "battery", pmu_battery_percent());
        cJSON_AddBoolToObject(o, "charging", pmu_is_charging());
        cJSON_AddBoolToObject(o, "stop", true);
        return json(r, o);
    }
    bool dogs = !strncmp(path, "/zampe/", 7), waste = !strncmp(path, "/raccolta/", 9);
    if ((dogs || waste) && (strstr(path, "/api/state") || strstr(path, "/api/export")))
    {
        cJSON *o = life_snapshot(dogs ? LIFE_ZAMPE : LIFE_RACCOLTA);
        if (strstr(path, "/api/export"))
        {
            cJSON_DeleteItemFromObjectCaseSensitive(o, "runtime");
            cJSON_DeleteItemFromObjectCaseSensitive(o, "save_error");
            cJSON_DeleteItemFromObjectCaseSensitive(o, "saving");
            cJSON_DeleteItemFromObjectCaseSensitive(o, "revision");
        }
        return json(r, o);
    }
    if (!strcmp(path, "/api/state"))
        return state(r);
    return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "Pagina non trovata");
}
static esp_err_t post(httpd_req_t *r)
{
    if (!strcmp(r->uri, "/api/pair"))
    {
        if (!origin(r))
            return denied(r);
        int64_t now = esp_timer_get_time();
        if (now - pair_window > 60000000)
        {
            pair_window = now;
            pair_failures = 0;
        }
        if (pair_failures >= 8)
        {
            httpd_resp_set_status(r, "429 Too Many Requests");
            return httpd_resp_sendstr(r, "Riprova tra un minuto");
        }
        cJSON *j = body(r);
        bool ok = !strcmp(life_string(j, "code"), code) && strlen(code) == 6;
        cJSON_Delete(j);
        if (!ok)
        {
            pair_failures++;
            return denied(r);
        }
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "token", token);
        return json(r, out);
    }
    if (!authorized(r))
        return denied(r);
    if (strstr(r->uri, "/api/kit/off"))
    {
        lite_post(LITE_WEB, 0, 0);
        return json(r, cJSON_CreateObject());
    }
    cJSON *j = body(r);
    if (!cJSON_IsObject(j))
    {
        cJSON_Delete(j);
        return bad(r, "Richiesta non valida");
    }
    char error[128] = "Comando non riconosciuto";
    bool ok = false;
    if (!strcmp(r->uri, "/zampe/api/action") || !strcmp(r->uri, "/raccolta/api/action"))
        ok = life_command(r->uri[1] == 'z' ? LIFE_ZAMPE : LIFE_RACCOLTA, j, error, sizeof(error));
    else if (!strcmp(r->uri, "/api/action"))
    {
        const char *op = life_string(j, "op");
#ifdef LITE_QA
        if (!strcmp(op, "qa.touch")) {
            int x=life_number(j,"x",-1), y=life_number(j,"y",-1);
            if (x>=0 && x<board_info()->width && y>=0 && y<board_info()->height) {
                lite_post(LITE_QA_TOUCH,x,(y<<1)|cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"pressed")));ok=true;
            }
        }
        if (!strcmp(op, "qa.key")) {
            int key=life_number(j,"key",-1), event=life_number(j,"event",-1);
            if (key>=0 && key<=1 && event>=0 && event<=1) {lite_post(LITE_BUTTON,key,event);ok=true;}
        }
#endif
        if (!strcmp(op, "settings"))
        {
            int b = life_number(j, "brightness", lite_settings.brightness),
                m = life_number(j, "web_minutes", lite_settings.web_minutes);
            if (b >= 10 && b <= 120 && m >= 1 && m <= 120)
            {
                lite_settings.brightness = (uint8_t)b;
                lite_settings.web_minutes = (uint16_t)m;
                if (cJSON_HasObjectItem(j, "aod"))
                    lite_settings.aod = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "aod"));
                lite_save();
                ok = true;
            }
            else
                snprintf(error, sizeof(error), "Impostazioni non valide");
        }
        if (!strcmp(op, "alarm.save"))
        {
            int h = life_number(j, "hour", -1), m = life_number(j, "minute", -1), days = life_number(j, "days", -1);
            uint32_t id = (uint32_t)life_number(j, "id", 0);
            alarm_t a = {.radio = -1};
            if (id)
                alarm_get(id, &a);
            if (h >= 0 && h < 24 && m >= 0 && m < 60 && days >= 0 && days <= 127 &&
                strlen(life_string(j, "label")) < ALARM_LABEL_MAX)
            {
                a.id = id;
                a.hour = (uint8_t)h;
                a.minute = (uint8_t)m;
                a.days = (uint8_t)days;
                a.enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "enabled"));
                snprintf(a.label, sizeof(a.label), "%s", life_string(j, "label"));
                ok = alarm_save(&a) == ESP_OK;
            }
            else
                snprintf(error, sizeof(error), "Sveglia non valida");
        }
        if (!strcmp(op, "alarm.delete"))
            ok = alarm_delete((uint32_t)life_number(j, "id", 0)) == ESP_OK;
        if (!strcmp(op, "alarm.stop"))
        {
            lite_alert_stop(false);
            ok = true;
        }
        if (!strcmp(op, "alarm.snooze"))
        {
            lite_alert_stop(true);
            ok = true;
        }
        if (!strcmp(op, "clock.set"))
        {
            const cJSON *utc = cJSON_GetObjectItemCaseSensitive(j, "utc");
            if (cJSON_IsNumber(utc) && utc->valuedouble >= 1735689600.0 && utc->valuedouble < 4102444800.0)
            {
                clock_set_utc((time_t)utc->valuedouble);
                ok = true;
            }
        }
        if (!strcmp(op, "event.add"))
        {
            const cJSON *start = cJSON_GetObjectItemCaseSensitive(j, "start"),
                        *end = cJSON_GetObjectItemCaseSensitive(j, "end");
            if (cJSON_IsNumber(start) && cJSON_IsNumber(end) && start->valuedouble >= 1735689600.0 &&
                end->valuedouble > start->valuedouble && strlen(life_string(j, "title")) > 0 &&
                strlen(life_string(j, "title")) < GD_TITLE)
            {
                gd_event_t event = {.start = (ics_time_t)start->valuedouble,
                                    .end = (ics_time_t)end->valuedouble,
                                    .remind = -1,
                                    .color = 0x70D5B0};
                snprintf(event.title, sizeof(event.title), "%s", life_string(j, "title"));
                ok = gd_event_put(&event) != 0;
            }
            else
                snprintf(error, sizeof(error), "Evento non valido");
        }
        if (!strcmp(op, "return"))
        {
            lite_post(LITE_EXIT, 0, 0);
            ok = true;
        }
    }
    cJSON_Delete(j);
    if (!ok)
        return bad(r, error);
    lite_post(LITE_DIRTY, 0, 0);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "accepted", true);
    return json(r, out);
}
static void network(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *ip = data;
        snprintf(address, sizeof(address), "http://" IPSTR "/", IP2STR(&ip->ip_info.ip));
        lite_post(LITE_DIRTY, 0, 0);
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        snprintf(address, sizeof(address), "RETE NON CONNESSA");
        atomic_store(&retry_at, esp_timer_get_time() + 15000000);
        lite_post(LITE_DIRTY, 0, 0);
    }
}
bool lite_web_running(void)
{
    return atomic_load(&running);
}
const char *lite_web_code(void)
{
    return code;
}
void lite_web_address(char *out, unsigned size)
{
    snprintf(out, size, "%s", address);
}
void lite_web_set(bool enabled)
{
    if (enabled == lite_web_running())
        return;
    if (!enabled)
    {
        atomic_store(&running, false);
        if (server)
        {
            httpd_stop(server);
            server = NULL;
        }
        if (handlers)
        {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);
            handlers = false;
        }
        esp_wifi_stop();
        esp_wifi_deinit();
        if (netif)
        {
            esp_netif_destroy_default_wifi(netif);
            netif = NULL;
        }
        address[0] = code[0] = 0;
        atomic_store(&retry_at, 0);
        lite_post(LITE_DIRTY, 0, 0);
        return;
    }
    typedef struct
    {
        char ssid[33], password[65];
    } known_t;
    known_t known[8] = {0};
    char preferred[33] = "";
    nvs_handle_t h;
    size_t n = sizeof(known);
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_blob(h, "known", known, &n);
        size_t len = sizeof(preferred);
        nvs_get_str(h, "preferred", preferred, &len);
        nvs_close(h);
    }
    int which = 0;
    for (int i = 0; i < 8; i++)
        if (preferred[0] && !strncmp(known[i].ssid, preferred, 33))
            which = i;
    if (!known[which].ssid[0])
    {
        snprintf(address, sizeof(address), "CONFIGURA WI-FI NEL SISTEMA");
        lite_post(LITE_DIRTY, 0, 0);
        return;
    }
    if (nvs_open("web", NVS_READWRITE, &h) != ESP_OK)
        return;
    n = sizeof(token);
    if (nvs_get_str(h, "token", token, &n) != ESP_OK || strlen(token) != 32)
    {
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        for (int i = 0; i < 16; i++)
            snprintf(token + i * 2, 3, "%02x", random[i]);
        if (nvs_set_str(h, "token", token) != ESP_OK || nvs_commit(h) != ESP_OK)
        {
            nvs_close(h);
            return;
        }
    }
    nvs_close(h);
    snprintf(code, sizeof(code), "%06u", (unsigned)(esp_random() % 1000000));
    pair_failures = 0;
    pair_window = esp_timer_get_time();
    esp_netif_init();
    esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE)
        return;
    netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK)
    {
        if (netif)
        {
            esp_netif_destroy_default_wifi(netif);
            netif = NULL;
        }
        snprintf(address, sizeof(address), "WI-FI NON DISPONIBILE");
        return;
    }
    atomic_store(&running, true);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network, NULL, &wifi_handler);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network, NULL, &ip_handler);
    handlers = true;
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, known[which].ssid, strnlen(known[which].ssid, 32));
    memcpy(config.sta.password, known[which].password, strnlen(known[which].password, 64));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &config);
    err = esp_wifi_start();
    if (err == ESP_OK)
    {
        /* Select a reachable configured network. The first saved network may
         * belong to a different city; do not pin Lite to that entry forever. */
        wifi_scan_config_t scan = {.show_hidden=true};
        if (esp_wifi_scan_start(&scan,true)==ESP_OK) {
            uint16_t count=20;
            wifi_ap_record_t *aps=heap_caps_calloc(count,sizeof(*aps),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(aps && esp_wifi_scan_get_ap_records(&count,aps)==ESP_OK) {
                int best=-1000;
                for(uint16_t i=0;i<count;i++)for(int k=0;k<8;k++) {
                    if(!known[k].ssid[0] || strncmp((const char *)aps[i].ssid,known[k].ssid,33))continue;
                    int score=aps[i].rssi+(!strncmp(known[k].ssid,preferred,33)?100:0);
                    if(score>best){best=score;which=k;}
                }
                memset(&config,0,sizeof(config));
                memcpy(config.sta.ssid,known[which].ssid,strnlen(known[which].ssid,32));
                memcpy(config.sta.password,known[which].password,strnlen(known[which].password,64));
                esp_wifi_set_config(WIFI_IF_STA,&config);
            }
            free(aps);
            esp_wifi_clear_ap_list();
        }
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
        err = esp_wifi_connect();
    }
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.uri_match_fn = httpd_uri_match_wildcard;
    http.stack_size = 8192;
    http.max_uri_handlers = 4;
    http.lru_purge_enable = true;
    http.recv_wait_timeout = 5;
    http.send_wait_timeout = 5;
    if (err == ESP_OK)
        err = httpd_start(&server, &http);
    if (err != ESP_OK)
    {
        lite_web_set(false);
        snprintf(address, sizeof(address), "AVVIO WI-FI FALLITO");
        return;
    }
    httpd_uri_t g = {.uri = "/*", .method = HTTP_GET, .handler = get},
                p = {.uri = "/*", .method = HTTP_POST, .handler = post};
    httpd_register_uri_handler(server, &g);
    httpd_register_uri_handler(server, &p);
    snprintf(address, sizeof(address), "CONNESSIONE IN CORSO");
    atomic_store(&used_at, esp_timer_get_time());
    lite_post(LITE_DIRTY, 0, 0);
}
void lite_web_tick(void)
{
    if (!lite_web_running())
        return;
    int64_t now = esp_timer_get_time();
    if (now - atomic_load(&used_at) > (int64_t)lite_settings.web_minutes * 60000000)
    {
        lite_web_set(false);
        return;
    }
    int64_t retry = atomic_load(&retry_at);
    if (retry && now >= retry)
    {
        atomic_store(&retry_at, 0);
        esp_wifi_connect();
    }
}
