#include "services/wifi.h"
#include "services/storage.h"

#include "core/state.h"
#include "core/sys.h"

#include "hardware/board.h"
#include "hardware/sdcard.h"

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wifi";

#define CONNECTED_BIT     BIT0
#define FAILED_BIT        BIT1
#define MAX_RETRIES       3
#define OFF_DELAY_US      (8 * 1000 * 1000)
#define NVS_NAMESPACE     "wifi"
#define NVS_KEY           "known"

static EventGroupHandle_t s_events = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static SemaphoreHandle_t s_scan_mutex = NULL;   /* one scan at a time: a second one cancels the first */
static esp_netif_t *s_sta = NULL;
static esp_netif_t *s_ap = NULL;
static esp_timer_handle_t s_off_timer = NULL;
static bool s_started = false;
static int s_fast_refs = 0;   /* wifi_set_fast() holders */
static bool s_sta_wanted = false;
static bool s_ap_on = false;
static int s_retries = 0;
static int s_refs = 0;
static unsigned s_radio_refs; /* ESP-NOW needs STA even alongside the fallback AP. */
static unsigned s_scans;       /* STA must stay enabled through concurrent radio owners. */
static int s_last_reason = 0;   /* wifi_err_reason_t of the last disconnection */
static uint8_t s_home_channel = 0;   /* channel of the last network joined */
static esp_pm_lock_handle_t s_ap_lock = NULL;   /* no light sleep while our access point runs */

static EXT_RAM_BSS_ATTR wifi_known_t s_known[WIFI_KNOWN_MAX];
static int s_known_count = 0;
static char s_preferred[33];

/* ------------------------------------------------------ known networks */

static esp_err_t known_save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);

    if (err != ESP_OK)
    {
        return err;
    }

    if (s_known_count == 0)
    {
        err = nvs_erase_key(nvs, NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            err = ESP_OK;
        }
    }
    else
    {
        err = nvs_set_blob(nvs, NVS_KEY, s_known, sizeof(wifi_known_t) * (size_t)s_known_count);
    }

    if (err == ESP_OK)
    {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot save known networks: %s", esp_err_to_name(err));
    }
    return err;
}

static void known_load(void)
{
    nvs_handle_t nvs;
    s_known_count = 0;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
    {
        return;
    }

    size_t preferred_size = sizeof(s_preferred);
    if (nvs_get_str(nvs, "preferred", s_preferred, &preferred_size) != ESP_OK) s_preferred[0] = '\0';
    size_t size = sizeof(s_known);

    if (nvs_get_blob(nvs, NVS_KEY, s_known, &size) == ESP_OK &&
        size <= sizeof(s_known) && size % sizeof(wifi_known_t) == 0)
    {
        s_known_count = (int)(size / sizeof(wifi_known_t));
        for (int i = 0; i < s_known_count; i++)
        {
            s_known[i].ssid[sizeof(s_known[i].ssid) - 1] = '\0';
            s_known[i].password[sizeof(s_known[i].password) - 1] = '\0';
        }
    }

    nvs_close(nvs);
}

static int known_find(const char *ssid)
{
    for (int i = 0; i < s_known_count; i++)
    {
        if (strcmp(s_known[i].ssid, ssid) == 0)
        {
            return i;
        }
    }

    return -1;
}

/* Add or update, newest first. Returns true if something changed. */
static bool known_put(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0')
    {
        return false;
    }

    int index = known_find(ssid);

    if (index >= 0 && strcmp(s_known[index].password, password != NULL ? password : "") == 0)
    {
        return false;
    }

    if (index < 0)
    {
        index = s_known_count < WIFI_KNOWN_MAX ? s_known_count++ : WIFI_KNOWN_MAX - 1;
    }

    wifi_known_t entry = {0};
    snprintf(entry.ssid, sizeof(entry.ssid), "%s", ssid);
    snprintf(entry.password, sizeof(entry.password), "%s", password != NULL ? password : "");

    /* Move to the front: the last network used is tried first. */
    memmove(&s_known[1], &s_known[0], sizeof(wifi_known_t) * (size_t)index);
    s_known[0] = entry;

    return true;
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text))
    {
        text++;
    }

    char *end = text + strlen(text);

    while (end > text && isspace((unsigned char)end[-1]))
    {
        *--end = '\0';
    }

    return text;
}

/* /sdcard/config/wifi.txt: "ssid = password" per line, # comments. */
static bool import_wifi_txt(void)
{
    char path[64];
    FILE *file = storage_config_path("wifi.txt", path, sizeof(path)) ? fopen(path, "r") : NULL;
    bool changed = false;

    if (file == NULL)
    {
        return false;
    }

    char line[160];

    while (fgets(line, sizeof(line), file) != NULL)
    {
        char *text = trim(line);
        char *eq = strchr(text, '=');

        if (text[0] == '#' || eq == NULL)
        {
            continue;
        }

        *eq = '\0';
        changed |= known_put(trim(text), trim(eq + 1));
    }

    fclose(file);

    return changed;
}

/* /networks.json of the Arduino launcher: [{"ssid": "...", "pwd": "..."}] */
static bool import_arduino_json(void)
{
    FILE *file = fopen(SDCARD_MOUNT "/networks.json", "r");
    bool changed = false;

    if (file == NULL)
    {
        return false;
    }

    char *text = calloc(1, 4096);

    if (text != NULL)
    {
        fread(text, 1, 4095, file);
        cJSON *root = cJSON_Parse(text);
        cJSON *item = NULL;

        cJSON_ArrayForEach(item, root)
        {
            const cJSON *ssid = cJSON_GetObjectItem(item, "ssid");
            const cJSON *pwd = cJSON_GetObjectItem(item, "pwd");

            if (cJSON_IsString(ssid))
            {
                changed |= known_put(ssid->valuestring, cJSON_IsString(pwd) ? pwd->valuestring : "");
            }
        }

        cJSON_Delete(root);
        free(text);
    }

    fclose(file);

    return changed;
}

/* --------------------------------------------------------------- radio */

/* Why the last connection attempt failed, in words for the user. */
const char *wifi_reason_text(int reason)
{
    switch (reason)
    {
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return "rete non trovata o segnale debole (solo 2,4 GHz)";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            return "password errata";
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_CONNECTION_FAIL:
            return "il router non risponde";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "segnale perso";
        case 0:
            return "nessun errore";
        default:
            return "errore di connessione";
    }
}

int wifi_last_reason(void)
{
    return s_last_reason;
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        if (s_sta_wanted)
        {
            esp_wifi_connect();
        }
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        const wifi_event_sta_disconnected_t *info = data;
        s_last_reason = info != NULL ? info->reason : 0;
        ESP_LOGW(TAG, "Disconnected from \"%.32s\": %s (%d)", info != NULL ? (const char *)info->ssid : "?",
                 wifi_reason_text(s_last_reason), s_last_reason);

        xEventGroupClearBits(s_events, CONNECTED_BIT);

        if (s_started && s_sta_wanted && s_retries < MAX_RETRIES)
        {
            s_retries++;
            esp_wifi_connect();
        }
        else
        {
            xEventGroupSetBits(s_events, FAILED_BIT);
        }

        state_set(STATE_WIFI, s_ap_on ? STATE_WIFI_AP : (s_sta_wanted ? STATE_WIFI_CONNECTING : STATE_WIFI_OFF));
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        s_retries = 0;
        s_last_reason = 0;
        ESP_LOGI(TAG, "Connected");
        uint8_t primary = 0;
        wifi_second_chan_t second;
        if (esp_wifi_get_channel(&primary, &second) == ESP_OK)
        {
            s_home_channel = primary;
        }
        xEventGroupSetBits(s_events, CONNECTED_BIT);
        state_set(STATE_WIFI, STATE_WIFI_CONNECTED);
    }
}

/*
 * Modem sleep between beacons: big saving, still fine for streaming. Not
 * with our access point on: an AP that naps drops the phones connected to it.
 */
static void apply_power_save(void)
{
    esp_wifi_set_ps((s_fast_refs > 0 || s_ap_on) ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
}

/* The mode we need: pure AP is the most stable, the station is added only when used. */
static wifi_mode_t wanted_mode(bool need_sta)
{
    if (!s_ap_on)
    {
        return WIFI_MODE_STA;
    }

    return (need_sta || s_radio_refs > 0 || s_sta_wanted || s_scans > 0 || wifi_is_connected()) ? WIFI_MODE_APSTA : WIFI_MODE_AP;
}

/*
 * The Wi-Fi driver takes ~75 KB of internal RAM: it exists only while the
 * radio is on (esp_wifi_init at the first use, esp_wifi_deinit when it goes
 * off), so a watch with Wi-Fi off keeps that memory for apps and Bluetooth.
 * The network interfaces live and die with it.
 */
static bool s_driver = false;

bool wifi_activity_snapshot(wifi_activity_t *out)
{
    if (out == NULL || s_mutex == NULL || xSemaphoreTake(s_mutex, 0) != pdTRUE) return false;
    *out = (wifi_activity_t){.driver=s_driver, .started=s_started, .access_point=s_ap_on,
        .holders=s_refs, .radio_holders=s_radio_refs, .fast_holders=s_fast_refs, .scans=s_scans};
    xSemaphoreGive(s_mutex);
    return true;
}

static bool driver_up(void)
{
    if (s_driver)
    {
        return true;
    }

    if (s_sta == NULL)
    {
        s_sta = esp_netif_create_default_wifi_sta();
    }

    if (s_ap == NULL)
    {
        s_ap = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&config);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Driver not started: %s", esp_err_to_name(err));
        sys_heap_log("wifi init");
        return false;
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);   /* our list is in NVS already */
    s_driver = true;

    return true;
}

static void driver_down(void)
{
    if (!s_driver)
    {
        return;
    }

    esp_wifi_deinit();
    s_driver = false;

    if (s_sta != NULL)
    {
        esp_netif_destroy_default_wifi(s_sta);
        s_sta = NULL;
    }

    if (s_ap != NULL)
    {
        esp_netif_destroy_default_wifi(s_ap);
        s_ap = NULL;
    }
}

/* False: no memory for the driver (the caller's job fails like a missing network). */
static bool radio_on(wifi_mode_t mode)
{
    if (!driver_up())
    {
        return false;
    }

    esp_err_t err = esp_wifi_set_mode(mode);
    if (err == ESP_OK && !s_started)
    {
        err = esp_wifi_start();
        if (err == ESP_OK) s_started = true;
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Radio mode/start failed: %s", esp_err_to_name(err));
        return false;
    }

    apply_power_save();

    return true;
}

/* Phones connected to our access point (scans and joins would move its channel). */
static bool ap_has_clients(void)
{
    wifi_sta_list_t list;

    return s_ap_on && esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0;
}

static void ap_set(bool on)
{
    if (on == s_ap_on)
    {
        return;
    }

    s_ap_on = on;

    if (s_ap_lock == NULL)
    {
        return;
    }

    if (on)
    {
        esp_pm_lock_acquire(s_ap_lock);
    }
    else
    {
        esp_pm_lock_release(s_ap_lock);
    }
}

static void radio_off(void)
{
    if (!s_started)
    {
        return;
    }

    s_sta_wanted = false;
    ap_set(false);
    xEventGroupClearBits(s_events, CONNECTED_BIT | FAILED_BIT);
    esp_wifi_stop();
    s_started = false;
    driver_down();
    state_set(STATE_WIFI, STATE_WIFI_OFF);
    ESP_LOGI(TAG, "Off");
}

static void off_job(void *arg)
{
    (void)arg;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_refs == 0)
    {
        radio_off();
    }

    xSemaphoreGive(s_mutex);
}

/* esp_timer task: esp_wifi_stop() blocks, so it runs in the sys worker. */
static void off_timer_cb(void *arg)
{
    (void)arg;
    sys_post(off_job, NULL);
}

/* Join one network. Called with the mutex held. */
static esp_err_t join(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32 ||
        (password != NULL && strlen(password) > 64)) return ESP_ERR_INVALID_ARG;
    wifi_config_t config = {0};
    /* ESP fields allow full-length SSIDs/PSKs without a trailing NUL. */
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    if (password != NULL) memcpy(config.sta.password, password, strlen(password));
    config.sta.threshold.authmode = (password != NULL && password[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    config.sta.listen_interval = 3;   /* wake for every 3rd beacon: less power */

    if (!radio_on(wanted_mode(true)))
    {
        return ESP_ERR_NO_MEM;
    }

    esp_wifi_disconnect();
    xEventGroupClearBits(s_events, CONNECTED_BIT | FAILED_BIT);
    s_retries = 0;
    s_sta_wanted = true;
    esp_err_t configured = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (configured != ESP_OK) {
        s_sta_wanted = false;
        state_set(STATE_WIFI, s_ap_on ? STATE_WIFI_AP : STATE_WIFI_OFF);
        return configured;
    }
    state_set(STATE_WIFI, STATE_WIFI_CONNECTING);

    ESP_LOGI(TAG, "Joining \"%s\"", ssid);
    esp_err_t connecting = esp_wifi_connect();
    /* A queued disconnect event may already have started the reconnect. */
    if (connecting != ESP_OK && connecting != ESP_ERR_WIFI_CONN) {
        s_sta_wanted = false;
        state_set(STATE_WIFI, s_ap_on ? STATE_WIFI_AP : STATE_WIFI_OFF);
        return connecting;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAILED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & CONNECTED_BIT)
    {
        return ESP_OK;
    }

    s_sta_wanted = false;
    esp_wifi_disconnect();

    if (s_ap_on)
    {
        radio_on(wanted_mode(false));   /* back to a pure access point */
    }

    return ESP_ERR_TIMEOUT;
}

/* Scan and join the strongest known network. Called with the mutex held. */
static esp_err_t join_best_known(uint32_t timeout_ms)
{
    if (s_known_count == 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    wifi_ap_t *found = calloc(WIFI_SCAN_MAX, sizeof(wifi_ap_t));
    int count = 0;

    if (found != NULL)
    {
        xSemaphoreGive(s_mutex);
        count = wifi_scan(found, WIFI_SCAN_MAX);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }

    /* Another job joined while we scanned without the mutex (its connection
     * also made our scan fail): joining again would disconnect it. */
    if (wifi_is_connected())
    {
        free(found);
        return ESP_OK;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;

    /* An explicit user selection wins over signal strength while available. */
    int preferred = known_find(s_preferred);
    bool preferred_heard = count == 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(found[i].ssid, s_preferred) == 0) preferred_heard = true;
    }
    if (preferred >= 0 && preferred_heard) {
        err = join(s_known[preferred].ssid, s_known[preferred].password, timeout_ms);
    }

    /* Strongest first (scan order), only networks we know. */
    for (int i = 0; i < count && err != ESP_OK; i++)
    {
        int k = known_find(found[i].ssid);

        if (k >= 0 && !(k == preferred && preferred_heard))
        {
            err = join(s_known[k].ssid, s_known[k].password, timeout_ms);
        }
    }

    /* Hidden networks do not show in a scan: try the most recent one blindly. */
    if (err != ESP_OK && count == 0)
    {
        err = join(s_known[0].ssid, s_known[0].password, timeout_ms);
    }

    free(found);

    return err;
}

/* -------------------------------------------------------------- public */

esp_err_t wifi_service_init(void)
{
    s_events = xEventGroupCreate();
    s_mutex = xSemaphoreCreateMutex();
    s_scan_mutex = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    /* The driver and the interfaces start with the radio (driver_up). */

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL);

    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "wifi_ap", &s_ap_lock);

    const esp_timer_create_args_t timer_args = {.callback = off_timer_cb, .name = "wifi_off"};
    esp_timer_create(&timer_args, &s_off_timer);

    known_load();

    if (storage_dir() != NULL)
    {
        bool changed = import_wifi_txt();
        if (sdcard_is_mounted())
        {
            changed |= import_arduino_json();
        }

        if (changed)
        {
            known_save();
        }
    }

    ESP_LOGI(TAG, "%d known networks", s_known_count);
    state_set(STATE_WIFI, STATE_WIFI_OFF);

    return ESP_OK;
}

esp_err_t wifi_acquire(uint32_t timeout_ms)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_refs++;
    esp_timer_stop(s_off_timer);

    esp_err_t err = ESP_OK;

    if (!wifi_is_connected())
    {
        /* Background jobs wait: joining now would kick the phone off our access point. */
        err = ap_has_clients() ? ESP_ERR_INVALID_STATE : join_best_known(timeout_ms);
    }

    if (err != ESP_OK)
    {
        s_refs--;

        if (s_refs == 0)
        {
            esp_timer_start_once(s_off_timer, 1000);
        }
    }

    xSemaphoreGive(s_mutex);

    return err;
}

esp_err_t wifi_radio_acquire(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_refs++;
    s_radio_refs++;
    esp_timer_stop(s_off_timer);
    esp_err_t err = radio_on(wanted_mode(false)) ? ESP_OK : ESP_ERR_NO_MEM;
    if (err != ESP_OK)
    {
        s_refs--;
        s_radio_refs--;
        if (s_refs == 0)
        {
            esp_timer_start_once(s_off_timer, 1000);
        }
    }
    xSemaphoreGive(s_mutex);
    return err;
}

uint8_t wifi_radio_channel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t second;
    if (!s_started || esp_wifi_get_channel(&primary, &second) != ESP_OK)
    {
        return 0;
    }
    return primary;
}

uint8_t wifi_home_channel(void)
{
    return s_home_channel;
}

bool wifi_radio_set_channel(uint8_t channel)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_started && !s_ap_on && !wifi_is_connected() && channel >= 1 && channel <= 13 &&
              esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK;
    xSemaphoreGive(s_mutex);
    return ok;
}

void wifi_radio_release(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_radio_refs > 0)
    {
        s_radio_refs--;
        if (s_refs > 0 && --s_refs == 0)
        {
            esp_timer_stop(s_off_timer);
            esp_timer_start_once(s_off_timer, OFF_DELAY_US);
        }
        if (s_ap_on) radio_on(wanted_mode(false));
    }
    xSemaphoreGive(s_mutex);
}

void wifi_release(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_refs > 0 && --s_refs == 0)
    {
        esp_timer_stop(s_off_timer);
        esp_timer_start_once(s_off_timer, OFF_DELAY_US);
    }

    xSemaphoreGive(s_mutex);
}

void wifi_set_fast(bool fast)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int before = s_fast_refs;
    s_fast_refs += fast ? 1 : (s_fast_refs > 0 ? -1 : 0);

    if (s_started && (before == 0) != (s_fast_refs == 0))
    {
        apply_power_save();
    }

    xSemaphoreGive(s_mutex);
}

#if CONFIG_WATCH_DEV_CONSOLE
void wifi_diagnostic_status(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    wifi_sta_list_t clients = {0};
    if (s_ap_on) esp_wifi_ap_get_sta_list(&clients);
    char ip[20] = ""; wifi_get_ip(ip, sizeof(ip));
    printf("@WIFI connected=%d ap=%d clients=%d radio_refs=%u known=%d reason=%d ip=%s\n",
        wifi_is_connected(), s_ap_on, clients.num, s_radio_refs, s_known_count, s_last_reason, ip);
    for (int i = 0; i < s_known_count; i++) printf("@WIFI ssid=%s\n", s_known[i].ssid);
    xSemaphoreGive(s_mutex);
}
void wifi_diagnostic_drop(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_sta_wanted = false;
    esp_wifi_disconnect();
    xSemaphoreGive(s_mutex);
}
#endif

bool wifi_is_connected(void)
{
    return s_events != NULL && (xEventGroupGetBits(s_events) & CONNECTED_BIT) != 0;
}

void wifi_current_ssid(char *out, size_t size)
{
    wifi_ap_record_t info;

    if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&info) == ESP_OK)
    {
        snprintf(out, size, "%s", (const char *)info.ssid);
    }
    else if (size > 0)
    {
        out[0] = '\0';
    }
}

int wifi_rssi(void)
{
    wifi_ap_record_t info;

    if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&info) == ESP_OK)
    {
        return info.rssi;
    }

    return 0;
}

bool wifi_get_ip(char *out, size_t size)
{
    esp_netif_ip_info_t info;
    esp_netif_t *netif = wifi_is_connected() ? s_sta : (s_ap_on ? s_ap : NULL);

    if (netif == NULL || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0)
    {
        return false;
    }

    snprintf(out, size, IPSTR, IP2STR(&info.ip));

    return true;
}

/* One scan: the raw records (free them), *found their count. NULL if none. */
static wifi_ap_record_t *scan_run(bool hidden, uint16_t *found)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool was_started = s_started;
    s_scans++;
    radio_on(wanted_mode(true));
    xSemaphoreGive(s_mutex);

    wifi_scan_config_t scan = {.show_hidden = hidden};
    wifi_ap_record_t *records = NULL;
    *found = 0;

    /* Two jobs asking for the network at once (weather + market) used to scan
     * together: the driver ends the first scan early and both found nothing. */
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    esp_err_t scan_err = esp_wifi_scan_start(&scan, true);
    if (scan_err != ESP_OK)
    {
        ESP_LOGW(TAG, "Scan not started: %s", esp_err_to_name(scan_err));
    }
    else
    {
        uint16_t count = WIFI_SCAN_MAX;
        records = calloc(count, sizeof(wifi_ap_record_t));
        if (records != NULL)
        {
            esp_wifi_scan_get_ap_records(&count, records);
            *found = count;
        }
        else
        {
            esp_wifi_clear_ap_list();
        }
    }
    xSemaphoreGive(s_scan_mutex);

    /* Switched on just for the scan and nobody else needs it: off again soon. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_scans--;
    if (s_ap_on)
    {
        radio_on(wanted_mode(false));   /* back to a pure access point */
    }
    if (!was_started && s_refs == 0)
    {
        esp_timer_stop(s_off_timer);
        esp_timer_start_once(s_off_timer, OFF_DELAY_US);
    }
    xSemaphoreGive(s_mutex);
    return records;
}

int wifi_scan(wifi_ap_t *out, int max)
{
    uint16_t found = 0;
    wifi_ap_record_t *records = scan_run(false, &found);
    int count = 0;
    for (int i = 0; records != NULL && i < found && count < max; i++)
    {
        const char *ssid = (const char *)records[i].ssid;
        bool duplicate = ssid[0] == '\0';
        for (int j = 0; j < count && !duplicate; j++)
        {
            duplicate = strcmp(out[j].ssid, ssid) == 0;
        }
        if (duplicate)
        {
            continue;
        }
        snprintf(out[count].ssid, sizeof(out[count].ssid), "%s", ssid);
        out[count].rssi = records[i].rssi;
        out[count].open = records[i].authmode == WIFI_AUTH_OPEN;
        out[count].known = known_find(ssid) >= 0;
        count++;
    }
    free(records);
    ESP_LOGI(TAG, "Scan: %d networks", count);
    return count;
}

int wifi_scan_detail(wifi_ap_detail_t *out, int max)
{
    uint16_t found = 0;
    wifi_ap_record_t *records = scan_run(true, &found);
    int count = 0;
    for (int i = 0; records != NULL && i < found && count < max; i++)
    {
        wifi_ap_detail_t *d = &out[count++];
        snprintf(d->ssid, sizeof(d->ssid), "%s", (const char *)records[i].ssid);
        memcpy(d->bssid, records[i].bssid, 6);
        d->channel = records[i].primary;
        d->rssi = records[i].rssi;
        d->auth = (uint8_t)records[i].authmode;
    }
    free(records);
    return count;
}

static esp_err_t save_preferred_locked(const char *ssid)
{
    if (strcmp(s_preferred, ssid) == 0) return ESP_OK;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = ssid[0] != '\0' ? nvs_set_str(nvs, "preferred", ssid) : nvs_erase_key(nvs, "preferred");
        if (err == ESP_ERR_NVS_NOT_FOUND && ssid[0] == '\0') err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err == ESP_OK) snprintf(s_preferred, sizeof(s_preferred), "%s", ssid);
    return err;
}

esp_err_t wifi_set_preferred(const char *ssid)
{
    if (ssid == NULL || strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ssid[0] != '\0' && known_find(ssid) < 0 ? ESP_ERR_NOT_FOUND : save_preferred_locked(ssid);
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t wifi_connect_new(const char *ssid, const char *password, uint32_t timeout_ms)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_refs++;
    esp_timer_stop(s_off_timer);

    esp_err_t err = join(ssid, password, timeout_ms);

    if (err == ESP_OK)
    {
        known_put(ssid, password);
        err = known_save();
        if (err == ESP_OK && save_preferred_locked(ssid) != ESP_OK)
            ESP_LOGW(TAG, "Connected; preferred network could not be persisted");
    }
    if (err != ESP_OK && --s_refs == 0)
    {
        esp_timer_start_once(s_off_timer, 1000);
    }

    xSemaphoreGive(s_mutex);

    return err;
}

void wifi_preferred_ssid(char *out, size_t size)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(out, size, "%s", s_preferred);
    xSemaphoreGive(s_mutex);
}

esp_err_t wifi_select_automatic(void)
{
    return wifi_set_preferred("");
}

/* Select a saved profile without exposing or rewriting its credentials. */
esp_err_t wifi_connect_saved(const char *ssid, uint32_t timeout_ms)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int index = known_find(ssid);
    if (index < 0) { xSemaphoreGive(s_mutex); return ESP_ERR_NOT_FOUND; }
    s_refs++;
    esp_timer_stop(s_off_timer);
    esp_err_t err = join(s_known[index].ssid, s_known[index].password, timeout_ms);
    if (err == ESP_OK && save_preferred_locked(ssid) != ESP_OK)
        ESP_LOGW(TAG, "Connected; preferred network could not be persisted");
    if (err != ESP_OK && --s_refs == 0) esp_timer_start_once(s_off_timer, 1000);
    xSemaphoreGive(s_mutex);
    return err;
}

int wifi_known_list(wifi_known_t *out, int max)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_known_count < max ? s_known_count : max;
    memcpy(out, s_known, sizeof(wifi_known_t) * (size_t)count);
    xSemaphoreGive(s_mutex);

    return count;
}

esp_err_t wifi_known_add(const char *ssid, const char *password)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32 ||
        (password != NULL && strlen(password) > 64))
    {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    known_put(ssid, password);
    esp_err_t err = known_save();
    if (err != ESP_OK)
    {
        known_load();
    }
    xSemaphoreGive(s_mutex);

    return err;
}

esp_err_t wifi_known_remove(const char *ssid)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int index = known_find(ssid);

    if (index >= 0)
    {
        memmove(&s_known[index], &s_known[index + 1], sizeof(wifi_known_t) * (size_t)(s_known_count - index - 1));
        s_known_count--;
    }

    esp_err_t err = index >= 0 ? known_save() : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK && index >= 0)
    {
        known_load();
    }

    xSemaphoreGive(s_mutex);

    return err;
}

bool wifi_known_any(void)
{
    return s_known_count > 0;
}

esp_err_t wifi_start_access_point(char *ssid_out, size_t size)
{
    wifi_config_t config = {0};
    /* "<name>-<board>", e.g. "Tamiro-AMOLED": the watch's own name, always the same. */
    int length = snprintf((char *)config.ap.ssid, sizeof(config.ap.ssid), "%s-%s", sys_nickname(),
                          board_info()->short_name);

    for (int i = 0; i < length && i < (int)sizeof(config.ap.ssid); i++)
    {
        if (config.ap.ssid[i] == ' ')
        {
            config.ap.ssid[i] = '-';
        }
    }

    config.ap.ssid_len = (uint8_t)length;
    config.ap.channel = s_home_channel != 0 ? s_home_channel : 6;
    config.ap.max_connection = 3;
    config.ap.authmode = WIFI_AUTH_OPEN;
    config.ap.pmf_cfg.required = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_refs++;
    esp_timer_stop(s_off_timer);
    bool was_ap = s_ap_on;
    ap_set(true);
    esp_err_t err = radio_on(wanted_mode(false)) ? esp_wifi_set_config(WIFI_IF_AP, &config) : ESP_FAIL;
    if (err != ESP_OK)
    {
        if (!was_ap) ap_set(false);
        if (--s_refs == 0) esp_timer_start_once(s_off_timer, 1000);
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "Access point failed: %s", esp_err_to_name(err));
        return err;
    }
    xSemaphoreGive(s_mutex);

    state_set(STATE_WIFI, STATE_WIFI_AP);
    snprintf(ssid_out, size, "%s", (const char *)config.ap.ssid);
    ESP_LOGI(TAG, "Access point \"%s\"", ssid_out);

    return ESP_OK;
}

void wifi_stop_access_point(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_ap_on)
    {
        ap_set(false);
        esp_wifi_set_mode(WIFI_MODE_STA);
        apply_power_save();
        state_set(STATE_WIFI, wifi_is_connected() ? STATE_WIFI_CONNECTED : STATE_WIFI_OFF);
    }

    xSemaphoreGive(s_mutex);
    wifi_release();
}
