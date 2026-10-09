#include "companion/ble_companion.h"

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

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

#define BLE_LINE_MAX            1536
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
static bool s_running = false;
static char *s_line = NULL;
static size_t s_line_len = 0;
static ble_music_t s_music;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_status_timer = NULL;
static esp_timer_handle_t s_slow_timer = NULL;    /* fast advertising -> slow */
static esp_timer_handle_t s_retry_timer = NULL;   /* NimBLE could not start: try again */
static bool s_fast = true;

static void start_advertising(void);

/* ------------------------------------------------------------- sending */

static void send_text(const char *text)
{
    uint16_t conn = s_conn;

    if (conn == BLE_HS_CONN_HANDLE_NONE || s_tx_handle == 0)
    {
        return;
    }

    size_t length = strlen(text);
    size_t chunk = ble_att_mtu(conn) > 3 ? ble_att_mtu(conn) - 3 : 20;

    for (size_t sent = 0; sent < length; sent += chunk)
    {
        size_t n = length - sent < chunk ? length - sent : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(text + sent, (uint16_t)n);

        if (om == NULL || ble_gatts_notify_custom(conn, s_tx_handle, om) != 0)
        {
            break;
        }
    }
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
    cJSON_AddStringToObject(json, "fw", "1.0");
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

static void handle_gb(const cJSON *msg)
{
    const char *type = json_string(msg, "t");

    if (strcmp(type, "notify") == 0)
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
                s_conn = event->connect.conn_handle;
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

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_advertising();
}

static void on_reset(int reason)
{
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

    if (settings_get()->ble_enabled && !s_running)
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

    s_running = false;

    if (s_conn != BLE_HS_CONN_HANDLE_NONE)
    {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ble_gap_adv_stop();
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
    bool enable = arg != NULL;

    if (enable != s_running)
    {
        ble_companion_enable(enable);
    }
}

/* Bluetooth switched on/off anywhere (settings page, web page, control center). */
static void settings_changed(const settings_t *settings)
{
    if (settings->ble_enabled != s_running)
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
