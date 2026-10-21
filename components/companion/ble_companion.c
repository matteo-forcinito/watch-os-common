#include "companion/ble_companion.h"
#include "ble_bridge.h"

#include "core/clock.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "core/state.h"
#include "hardware/board.h"
#include "hardware/pmu.h"
#include "services/notify.h"
#include "services/sound.h"
#include "services/time_sync.h"
#include "services/weather.h"
#include "services/web_server.h"
#include "services/wifi.h"
#include "services/storage.h"
#include "services/fw_update.h"
#include <stdatomic.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ble";

/* NimBLE store (bonds in NVS); declared here because its header does not. */
void ble_store_config_init(void);

#define BLE_LINE_MAX            2048   /* bridge requests carry up to 1400 bytes of body */
#define ADV_FAST_MIN        160     /* x 0.625 ms = 100 ms: easy to find while pairing */
#define ADV_FAST_MAX        240     /* 150 ms */
#define ADV_SLOW_MIN        1600    /* 1.0 s: cheap, the phone reconnects anyway */
#define ADV_SLOW_MAX        2400    /* 1.5 s */
#define ADV_FAST_US         (60LL * 1000 * 1000)
#define RETRY_US            (30LL * 1000 * 1000)
#define STATUS_PERIOD_US    (10LL * 60 * 1000 * 1000)

/* Nordic UART Service, 6E40000x-B5A3-F393-E0A9-E50E24DCCA9E (bytes little endian). */
static const ble_uuid128_t s_nus_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static uint16_t s_tx_handle = 0;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type = 0;
static atomic_bool s_running;
static SemaphoreHandle_t s_control_guard;
static char *s_line = NULL;
static size_t s_line_len = 0;
static ble_music_t s_music;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_status_timer = NULL;
static esp_timer_handle_t s_slow_timer = NULL;    /* fast advertising -> slow */
static esp_timer_handle_t s_retry_timer = NULL;   /* NimBLE could not start: try again */
static bool s_fast = true;
static atomic_uint s_control_epoch;
static bool s_control_lease;
static atomic_bool s_control_busy;

static void start_advertising(void);

/* ------------------------------------------------------------- sending */

static SemaphoreHandle_t s_send_lock;   /* one line at a time: chunks never interleave */

/* Sends a line in MTU-sized notifications. With retries, waits for free
 * buffers instead of dropping the rest (bridge answers stream several KB). */
static bool send_chunks(const char *text, int retries)
{
    uint16_t conn = s_conn;

    if (conn == BLE_HS_CONN_HANDLE_NONE || s_tx_handle == 0 || s_send_lock == NULL)
    {
        return false;
    }

    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    size_t length = strlen(text);
    size_t chunk = ble_att_mtu(conn) > 3 ? ble_att_mtu(conn) - 3 : 20;
    bool ok = true;

    for (size_t sent = 0; ok && sent < length; sent += chunk)
    {
        size_t n = length - sent < chunk ? length - sent : chunk;
        for (int attempt = 0;; attempt++)
        {
            /* The stack frees the buffer even when the notify fails. */
            struct os_mbuf *om = ble_hs_mbuf_from_flat(text + sent, (uint16_t)n);
            if (om != NULL && ble_gatts_notify_custom(conn, s_tx_handle, om) == 0)
            {
                break;
            }
            if (attempt >= retries || s_conn != conn)
            {
                ok = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    xSemaphoreGive(s_send_lock);
    return ok;
}

static void send_text(const char *text)
{
    send_chunks(text, 0);
}

static bool bridge_send(const char *line)
{
    return send_chunks(line, 50);
}

static void send_json(cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);

    if (text != NULL)
    {
        size_t length = strlen(text);
        char *line = malloc(length + 2);

        if (line != NULL)
        {
            memcpy(line, text, length);
            line[length] = '\n';
            line[length + 1] = '\0';
            send_text(line);
            free(line);
        }

        cJSON_free(text);
    }

    cJSON_Delete(json);
}

void ble_companion_send_status(void)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "t", "status");
    cJSON_AddNumberToObject(json, "bat", pmu_battery_percent() < 0 ? 100 : pmu_battery_percent());
    cJSON_AddNumberToObject(json, "chg", pmu_is_charging() ? 1 : 0);
    cJSON_AddNumberToObject(json, "volt", pmu_battery_mv() / 1000.0);
    send_json(json);
}

static void send_version(void)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "t", "ver");
    cJSON_AddStringToObject(json, "fw", fw_update_running_version());
    cJSON_AddNumberToObject(json, "control", 1);
    cJSON_AddNumberToObject(json, "bridge", 1);   /* app API over Bluetooth, see ble_bridge.c */
    cJSON_AddStringToObject(json, "hw", board_info()->name);
    send_json(json);
}

void ble_companion_music_command(const char *command)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "t", "music");
    cJSON_AddStringToObject(json, "n", command);
    send_json(json);
}

void ble_companion_call_command(const char *command)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "t", "call");
    cJSON_AddStringToObject(json, "n", command);
    send_json(json);
}

void ble_companion_find_phone(bool on)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "t", "findPhone");
    cJSON_AddBoolToObject(json, "n", on);
    send_json(json);
}

/* ----------------------------------------------------------- receiving */

static const char *json_string(const cJSON *object, const char *key)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(object, key));
    return value != NULL ? value : "";
}

/* OpenWeatherMap condition code (what Gadgetbridge sends) -> WMO code. */
static int owm_to_wmo(int code)
{
    if (code >= 200 && code < 300) return 95;
    if (code >= 300 && code < 400) return 53;
    if (code == 511 || (code >= 600 && code < 700)) return 73;
    if (code >= 520 && code < 600) return 81;
    if (code >= 500 && code < 600) return 63;
    if (code >= 700 && code < 800) return 45;
    if (code == 800) return 0;
    if (code == 801) return 1;
    if (code == 802) return 2;
    return 3;
}


/* Bootstrap only across an encrypted OS bond. Network work never runs in NimBLE. */
typedef struct {
    struct ble_npl_event event;
    uint32_t epoch;
    uint16_t conn;
    char request[33];
    char network[33];
    char password[65];
    bool provision_network;
    bool network_choice;
    cJSON *reply;
} control_job_t;
static void control_erase(void *data, size_t size) { volatile unsigned char *p = data; while (size--) *p++ = 0; }
static bool control_current(control_job_t *job) { return s_running && job->epoch == s_control_epoch; }
static bool control_trusted(control_job_t *job)
{
    struct ble_gap_conn_desc desc;
    return job->epoch == s_control_epoch && job->conn == s_conn &&
        ble_gap_conn_find(job->conn, &desc) == 0 && desc.sec_state.encrypted && desc.sec_state.bonded;
}
static void control_deliver(struct ble_npl_event *event)
{
    control_job_t *job = ble_npl_event_get_arg(event);
    if (control_trusted(job)) send_json(job->reply);
    else cJSON_Delete(job->reply);
    free(job);
    atomic_store(&s_control_busy, false);
}
static void control_stop(void *arg) { (void)arg; web_server_stop(); }
static void control_worker(void *arg)
{
    control_job_t *job = arg;
    bool acquired = false;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (control_current(job)) {
        portENTER_CRITICAL(&s_lock);
        bool held = s_control_lease;
        portEXIT_CRITICAL(&s_lock);
        if (job->network_choice && job->network[0] == '\0') err = wifi_select_automatic();
        else if (job->network[0] != '\0') {
            char previous[33] = "", preferred[33] = "";
            wifi_current_ssid(previous, sizeof(previous));
            wifi_preferred_ssid(preferred, sizeof(preferred));
            fw_update_phase_t phase = fw_update_phase();
            bool busy = storage_files_busy() || phase == FW_UPDATE_CHECKING ||
                phase == FW_UPDATE_WRITING || phase == FW_UPDATE_RESTARTING;
            err = busy ? ESP_ERR_INVALID_STATE : job->provision_network ?
                wifi_connect_new(job->network, job->password, 12000) : wifi_connect_saved(job->network, 12000);
            if (err == ESP_OK) wifi_release(); /* Web owns the lasting reference. */
            else if (!busy && previous[0] != '\0' && !wifi_is_connected()) {
                /* A failed explicit choice must not strand the previous session. */
                if (wifi_connect_saved(previous, 12000) == ESP_OK) wifi_release();
                wifi_set_preferred(preferred);
            }
        } else err = ESP_OK;
        if (err == ESP_OK) err = held ? ESP_OK : web_server_start();
        acquired = !held && err == ESP_OK;
    }
    control_erase(job->password, sizeof(job->password));
    portENTER_CRITICAL(&s_lock);
    bool current = job->epoch == s_control_epoch && job->conn == s_conn;
    if (current && acquired) s_control_lease = true;
    portEXIT_CRITICAL(&s_lock);
    if (!current && acquired) web_server_stop();
    job->reply = cJSON_CreateObject();
    cJSON_AddStringToObject(job->reply, "t", "control");
    cJSON_AddStringToObject(job->reply, "request", job->request);
    char code[7] = "", url[96] = "", ap[40] = "";
    bool recovery = false;
    if (err == ESP_OK && control_current(job) && web_server_pairing_snapshot(code, &recovery)) {
        web_server_url(url, sizeof(url));
        web_server_ap_name(ap, sizeof(ap));
        cJSON_AddStringToObject(job->reply, "url", url);
        cJSON_AddStringToObject(job->reply, "code", recovery ? "" : code);
        cJSON_AddStringToObject(job->reply, "ap", ap);
        cJSON_AddBoolToObject(job->reply, "recovery", recovery);
        char ssid[33] = "";
        wifi_current_ssid(ssid, sizeof(ssid));
        cJSON_AddStringToObject(job->reply, "ssid", ssid);
        wifi_preferred_ssid(ssid, sizeof(ssid));
        cJSON_AddStringToObject(job->reply, "preferred_network", ssid);
        cJSON_AddBoolToObject(job->reply, "wifi_connected", wifi_is_connected());
        cJSON_AddBoolToObject(job->reply, "network_control", true);
        cJSON_AddBoolToObject(job->reply, "network_provisioning", true);
        wifi_known_t *known = calloc(WIFI_KNOWN_MAX, sizeof(*known));
        cJSON *networks = cJSON_AddArrayToObject(job->reply, "networks");
        if (known != NULL) {
            int count = wifi_known_list(known, WIFI_KNOWN_MAX);
            for (int i = 0; i < count; i++) cJSON_AddItemToArray(networks, cJSON_CreateString(known[i].ssid));
            volatile unsigned char *wipe = (volatile unsigned char *)known;
            for (size_t i = 0; i < WIFI_KNOWN_MAX * sizeof(*known); i++) wipe[i] = 0;
            free(known);
        }
    } else cJSON_AddStringToObject(job->reply, "error",
        job->network[0] != '\0' ? "network_unavailable" : "web_unavailable");
    xSemaphoreTake(s_control_guard, portMAX_DELAY);
    if (control_current(job)) {
        ble_npl_event_init(&job->event, control_deliver, job);
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &job->event);
    } else {
        cJSON_Delete(job->reply); free(job); atomic_store(&s_control_busy, false);
    }
    xSemaphoreGive(s_control_guard);
    vTaskDelete(NULL);
}
static void control_request(const cJSON *msg)
{
    const char *request = json_string(msg, "request");
    if (strlen(request) != 32 || strspn(request, "0123456789abcdef") != 32) return;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn, &desc) != 0) return;
    if (!desc.sec_state.encrypted || !desc.sec_state.bonded) {
        ble_gap_security_initiate(s_conn);
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "t", "control");
        cJSON_AddStringToObject(out, "request", request);
        cJSON_AddStringToObject(out, "error", "pairing_required");
        send_json(out);
        return;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_control_busy, &expected, true)) return;
    const cJSON *network = cJSON_GetObjectItemCaseSensitive(msg, "network");
    if (network != NULL && (!cJSON_IsString(network) || strlen(network->valuestring) > 32)) {
        atomic_store(&s_control_busy, false); return;
    }
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(msg, "password");
    if (password != NULL && (!cJSON_IsString(password) || network == NULL ||
        network->valuestring[0] == '\0' || strlen(password->valuestring) > 64 ||
        (strlen(password->valuestring) > 0 && strlen(password->valuestring) < 8) ||
        (strlen(password->valuestring) == 64 && strspn(password->valuestring, "0123456789abcdefABCDEF") != 64))) {
        atomic_store(&s_control_busy, false); return;
    }
    control_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) { atomic_store(&s_control_busy, false); return; }
    job->epoch = s_control_epoch; job->conn = s_conn;
    memcpy(job->request, request, sizeof(job->request));
    job->network_choice = network != NULL;
    job->provision_network = password != NULL;
    if (password != NULL) snprintf(job->password, sizeof(job->password), "%s", password->valuestring);
    if (network != NULL) snprintf(job->network, sizeof(job->network), "%s", network->valuestring);
    if (!sys_task_create(control_worker, "ble_control", 4096, job, 2, SYS_CORE_ANY)) {
        control_erase(job->password, sizeof(job->password));
        free(job); atomic_store(&s_control_busy, false);
    }
}

static void handle_gb(const cJSON *msg)
{
    const char *type = json_string(msg, "t");

    if (strcmp(type, "control") == 0) { control_request(msg); }
    else if (strcmp(type, "api") == 0)
    {
        /* The whole API, like the paired browser: only across an encrypted bond. */
        struct ble_gap_conn_desc desc;
        uint16_t conn = s_conn;
        if (ble_gap_conn_find(conn, &desc) == 0 && desc.sec_state.encrypted && desc.sec_state.bonded)
        {
            ble_bridge_request(msg, conn);
        }
        else
        {
            ble_gap_security_initiate(conn);
        }
    }
    else if (strcmp(type, "notify") == 0)
    {
        notify_t n = {0};
        n.id = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "id"));
        snprintf(n.app, sizeof(n.app), "%s", json_string(msg, "src"));
        const char *title = json_string(msg, "title");
        snprintf(n.title, sizeof(n.title), "%s", title[0] != '\0' ? title : json_string(msg, "sender"));
        const char *body = json_string(msg, "body");
        snprintf(n.body, sizeof(n.body), "%s", body[0] != '\0' ? body : json_string(msg, "subject"));
        notify_add(&n);
    }
    else if (strcmp(type, "notify-") == 0)
    {
        notify_remove((uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "id")));
    }
    else if (strcmp(type, "call") == 0)
    {
        notify_call_t call = {0};
        call.active = strcmp(json_string(msg, "cmd"), "incoming") == 0;
        snprintf(call.name, sizeof(call.name), "%s", json_string(msg, "name"));
        snprintf(call.number, sizeof(call.number), "%s", json_string(msg, "number"));
        notify_set_call(&call);
    }
    else if (strcmp(type, "musicinfo") == 0)
    {
        portENTER_CRITICAL(&s_lock);
        s_music.valid = true;
        snprintf(s_music.artist, sizeof(s_music.artist), "%s", json_string(msg, "artist"));
        snprintf(s_music.track, sizeof(s_music.track), "%s", json_string(msg, "track"));
        portEXIT_CRITICAL(&s_lock);
        state_bump(STATE_MUSIC_VERSION);
    }
    else if (strcmp(type, "musicstate") == 0)
    {
        s_music.valid = true;
        s_music.playing = strcmp(json_string(msg, "state"), "play") == 0;
        state_bump(STATE_MUSIC_VERSION);
    }
    else if (strcmp(type, "weather") == 0)
    {
        weather_t w = {0};
        w.temp = (float)(cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "temp")) - 273.15);
        w.temp_max = (float)(cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "hi")) - 273.15);
        w.temp_min = (float)(cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "lo")) - 273.15);
        w.humidity = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "hum"));
        w.wind_kmh = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "wind"));
        w.code = owm_to_wmo((int)cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "code")));
        struct tm now;
        clock_local(&now);
        w.is_day = now.tm_hour >= 7 && now.tm_hour < 20;
        snprintf(w.city, sizeof(w.city), "%s", json_string(msg, "loc"));
        weather_set_from_phone(&w);
    }
    else if (strcmp(type, "find") == 0)
    {
        if (cJSON_IsTrue(cJSON_GetObjectItem(msg, "n")))
        {
            power_wake(POWER_WAKE_NOTIFICATION);
            sound_alarm_start();
        }
        else
        {
            sound_alarm_stop();
        }
    }
}

static void handle_line(char *line)
{
    /* Gadgetbridge prefixes commands with control characters (\x03, \x10). */
    while (*line != '\0' && (unsigned char)*line < 0x20)
    {
        line++;
    }

    char *set_time = strstr(line, "setTime(");

    if (set_time != NULL)
    {
        long long seconds = atoll(set_time + 8);

        if (seconds > 1700000000LL && settings_get()->auto_time)
        {
            clock_set_utc((time_t)seconds);
            time_sync_mark();
            ESP_LOGI(TAG, "Time set by the phone");
        }
    }

    char *gb = strstr(line, "GB(");

    if (gb != NULL)
    {
        char *start = gb + 3;
        char *end = strrchr(start, ')');

        if (end != NULL)
        {
            *end = '\0';
            cJSON *msg = cJSON_Parse(start);

            if (msg != NULL)
            {
                handle_gb(msg);
                cJSON *secret = cJSON_GetObjectItemCaseSensitive(msg, "password");
                if (cJSON_IsString(secret)) control_erase(secret->valuestring, strlen(secret->valuestring));
                cJSON_Delete(msg);
            }
        }
    }
}

static void receive(const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        char c = (char)data[i];

        if (c == '\n')
        {
            s_line[s_line_len] = '\0';
            handle_line(s_line);
            control_erase(s_line, s_line_len);
            s_line_len = 0;
        }
        else if (s_line_len < BLE_LINE_MAX - 1)
        {
            s_line[s_line_len++] = c;
        }
    }
}

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        return 0;
    }

    uint8_t buffer[256];
    uint16_t got = 0;

    if (ble_hs_mbuf_to_flat(ctxt->om, buffer, sizeof(buffer), &got) == 0)
    {
        receive(buffer, got);
    }

    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)ctxt;
    (void)arg;
    return 0;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_nus_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = tx_access,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

/* --------------------------------------------------------------- GAP */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0)
            {
                portENTER_CRITICAL(&s_lock);
                s_control_epoch++;
                s_conn = event->connect.conn_handle;
                portEXIT_CRITICAL(&s_lock);
                state_set(STATE_BLE, STATE_BLE_CONNECTED);
                ESP_LOGI(TAG, "Phone connected");

                /* Relaxed link: 100-200 ms interval, may skip 4 events -> rare radio wake-ups. */
                struct ble_gap_upd_params params = {
                    .itvl_min = 80,
                    .itvl_max = 160,
                    .latency = 4,
                    .supervision_timeout = 600,
                };
                ble_gap_update_params(s_conn, &params);
                esp_timer_start_periodic(s_status_timer, STATUS_PERIOD_US);
            }
            else
            {
                start_advertising();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            portENTER_CRITICAL(&s_lock);
            s_control_epoch++;
            bool release_control = s_control_lease;
            s_control_lease = false;
            portEXIT_CRITICAL(&s_lock);
            if (release_control && !sys_post(control_stop, NULL)) {
                /* Retain ownership so the next connection can reuse/release it. */
                portENTER_CRITICAL(&s_lock);
                s_control_lease = true;
                portEXIT_CRITICAL(&s_lock);
            }
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_line_len = 0;
            esp_timer_stop(s_status_timer);
            ESP_LOGI(TAG, "Phone disconnected (reason %d)", event->disconnect.reason);
            s_fast = true;
            start_advertising();
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_tx_handle && event->subscribe.cur_notify)
            {
                send_version();
                ble_companion_send_status();
            }
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "Link encrypted: %d", event->enc_change.status);
            break;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
        {
            /* The phone forgot us and pairs again: drop the old bond and accept. */
            struct ble_gap_conn_desc desc;

            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }

            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising();
            break;

        default:
            break;
    }

    return 0;
}

static void start_advertising(void)
{
    if (!s_running || s_conn != BLE_HS_CONN_HANDLE_NONE)
    {
        return;
    }

    const char *name = ble_svc_gap_device_name();

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)name;
    fields.name_len = (uint8_t)strlen(name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_hs_adv_fields response = {0};
    response.uuids128 = (ble_uuid128_t *)&s_nus_uuid;
    response.num_uuids128 = 1;
    response.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&response);

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = s_fast ? ADV_FAST_MIN : ADV_SLOW_MIN,
        .itvl_max = s_fast ? ADV_FAST_MAX : ADV_SLOW_MAX,
    };

    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);

    if (rc == 0 || rc == BLE_HS_EALREADY)
    {
        state_set(STATE_BLE, STATE_BLE_ADVERTISING);
    }
    else
    {
        ESP_LOGW(TAG, "Advertising failed: %d", rc);
    }
}

/* ------------------------------------------------- shared scanning */

/* Internal apps (Ombra) scan with this host instead of starting their own:
 * one NimBLE owner. Scanning shares the radio with advertising and the phone
 * connection (half duty cycle) and resumes after a Bluetooth restart. */
static int (*volatile s_scan_cb)(struct ble_gap_event *event, void *arg);
static volatile bool s_synced;
static int s_holds;   /* apps that need Bluetooth now (ble_companion_hold, sys worker only) */

static int scan_event(struct ble_gap_event *event, void *arg);

static void scan_begin(void)
{
    if (s_scan_cb == NULL || !s_running || !s_synced || ble_gap_disc_active())
    {
        return;
    }
    struct ble_gap_disc_params params = {
        .itvl = 0x00A0,     /* 100 ms */
        .window = 0x0050,   /* 50 ms: the rest for advertising and the phone */
        .passive = 0,       /* scan requests bring the names */
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, scan_event, NULL);
    if (rc != 0)
    {
        ESP_LOGW(TAG, "Scan start failed: %d", rc);
    }
}

static int scan_event(struct ble_gap_event *event, void *arg)
{
    int (*cb)(struct ble_gap_event *, void *) = s_scan_cb;
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE)
    {
        scan_begin();
    }
    else if (cb != NULL)
    {
        cb(event, arg);
    }
    return 0;
}

bool ble_companion_scan_start(int (*cb)(struct ble_gap_event *event, void *arg))
{
    s_scan_cb = cb;
    scan_begin();
    return s_running;
}

void ble_companion_scan_stop(void)
{
    s_scan_cb = NULL;
    if (s_running && s_synced && ble_gap_disc_active())
    {
        ble_gap_disc_cancel();
    }
}

bool ble_companion_scanning(void)
{
    return s_running && s_synced && s_scan_cb != NULL && ble_gap_disc_active();
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    start_advertising();
    scan_begin();
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "Host reset: %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();   /* returns after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

static void status_job(void *arg)
{
    (void)arg;
    ble_companion_send_status();
}

/* esp_timer task: build and send the message in the sys worker. */
static void status_timer_cb(void *arg)
{
    (void)arg;
    sys_post(status_job, NULL);
}

static void slow_job(void *arg)
{
    (void)arg;

    if (s_running && s_fast && s_conn == BLE_HS_CONN_HANDLE_NONE)
    {
        s_fast = false;
        ble_gap_adv_stop();
        start_advertising();
        ESP_LOGI(TAG, "Slow advertising");
    }
}

static void slow_timer_cb(void *arg)
{
    (void)arg;
    sys_post(slow_job, NULL);
}

static esp_err_t start(void);

static void retry_job(void *arg)
{
    (void)arg;

    if ((settings_get()->ble_enabled || s_holds > 0) && !s_running)
    {
        start();
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    sys_post(retry_job, NULL);
}

/* ------------------------------------------------------------- public */

static esp_err_t start(void)
{
    if (s_running)
    {
        return ESP_OK;
    }

    sys_heap_log("ble start");
    /* The S3 controller allocates a contiguous internal pool (~38 KiB).
     * Its binary asserts on allocation failure instead of returning an error.
     * Web/Wi-Fi may fragment the heap after boot: defer before entering it.
     * Keep room for the controller task and small internal allocations too. */
    if (sys_internal_largest() < 40 * 1024 || sys_internal_free() < 56 * 1024) {
        ESP_LOGW(TAG, "Bluetooth deferred: insufficient contiguous internal RAM; close heavy services or restart");
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, RETRY_US);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nimble_port_init();

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NimBLE init: %s (retrying in 30 s)", esp_err_to_name(err));
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, RETRY_US);
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /* "Just works" pairing with bonding: accepted if the phone asks for it. */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_services);
    ble_gatts_add_svcs(s_services);
    ble_svc_gap_device_name_set(settings_get()->device_name);

    s_running = true;
    s_fast = true;
    esp_timer_stop(s_slow_timer);
    esp_timer_start_once(s_slow_timer, ADV_FAST_US);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "Bluetooth on as \"%s\"", settings_get()->device_name);

    return ESP_OK;
}

static void stop(void)
{
    if (!s_running)
    {
        return;
    }

    xSemaphoreTake(s_control_guard, portMAX_DELAY);
    s_running = false;
    portENTER_CRITICAL(&s_lock);
    s_control_epoch++;
    bool release_control = s_control_lease;
    s_control_lease = false;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_control_guard);
    if (release_control) web_server_stop();

    if (s_conn != BLE_HS_CONN_HANDLE_NONE)
    {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ble_gap_adv_stop();
    if (s_synced && ble_gap_disc_active())
    {
        ble_gap_disc_cancel();
    }
    s_synced = false;
    esp_timer_stop(s_status_timer);
    esp_timer_stop(s_slow_timer);
    esp_timer_stop(s_retry_timer);

    if (nimble_port_stop() == 0)
    {
        nimble_port_deinit();
    }

    s_conn = BLE_HS_CONN_HANDLE_NONE;
    state_set(STATE_BLE, STATE_BLE_OFF);
    ESP_LOGI(TAG, "Bluetooth off");
}

static void apply_enabled_job(void *arg)
{
    bool enable = arg != NULL || s_holds > 0;

    if (enable != s_running)
    {
        ble_companion_enable(enable);
    }
}

static void hold_job(void *arg)
{
    s_holds += arg != NULL ? 1 : (s_holds > 0 ? -1 : 0);
    apply_enabled_job(settings_get()->ble_enabled ? (void *)1 : NULL);
}

void ble_companion_hold(bool on)
{
    while (!sys_post(hold_job, on ? (void *)1 : NULL))
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Bluetooth switched on/off anywhere (settings page, web page, control center). */
static void settings_changed(const settings_t *settings)
{
    if ((settings->ble_enabled || s_holds > 0) != s_running)
    {
        sys_post(apply_enabled_job, settings->ble_enabled ? (void *)1 : NULL);
    }
}

static void restart_job(void *arg)
{
    (void)arg;
    ble_companion_enable(false);
    ble_companion_enable(settings_get()->ble_enabled);
}

void ble_companion_restart(void)
{
    sys_post(restart_job, NULL);
}

esp_err_t ble_companion_init(void)
{
    if (s_control_guard == NULL) s_control_guard = xSemaphoreCreateMutex();
    if (s_control_guard == NULL) return ESP_ERR_NO_MEM;
    if (s_send_lock == NULL) s_send_lock = xSemaphoreCreateMutex();
    if (s_send_lock == NULL) return ESP_ERR_NO_MEM;
    ble_bridge_init(bridge_send);
    s_line = malloc(BLE_LINE_MAX);

    if (s_line == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t args = {.callback = status_timer_cb, .name = "ble_status"};
    esp_timer_create(&args, &s_status_timer);
    const esp_timer_create_args_t slow_args = {.callback = slow_timer_cb, .name = "ble_slow"};
    esp_timer_create(&slow_args, &s_slow_timer);
    const esp_timer_create_args_t retry_args = {.callback = retry_timer_cb, .name = "ble_retry"};
    esp_timer_create(&retry_args, &s_retry_timer);

    state_set(STATE_BLE, STATE_BLE_OFF);
    settings_add_listener(settings_changed);

    return settings_get()->ble_enabled ? start() : ESP_OK;
}

void ble_companion_enable(bool enable)
{
    if (enable)
    {
        start();
    }
    else
    {
        stop();
    }
}

bool ble_companion_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_companion_music(ble_music_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_music;
    portEXIT_CRITICAL(&s_lock);

    return out->valid;
}
