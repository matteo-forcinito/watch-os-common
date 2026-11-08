#ifndef WIFI_DRIVER_TEST_STUBS_H
#define WIFI_DRIVER_TEST_STUBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../wifi_channel.h"

typedef int esp_err_t;
enum { ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_WIFI_NOT_CONNECT = -2,
       ESP_ERR_INVALID_STATE = -3, ESP_ERR_INVALID_ARG = -4,
       ESP_ERR_NO_MEM = -5, ESP_ERR_TIMEOUT = -6, ESP_ERR_WIFI_CONN = -7 };
typedef struct { int slot; } esp_netif_t;
typedef int wifi_second_chan_t;
typedef unsigned EventBits_t;
typedef struct {
    struct {
        uint8_t ssid[32], password[64];
        struct { int authmode; } threshold;
        int sae_pwe_h2e, listen_interval;
    } sta;
} wifi_config_t;
typedef struct { bool show_hidden; } wifi_scan_config_t;
typedef struct { int unused; } wifi_ap_record_t;
typedef struct { int unused; } wifi_init_config_t;
typedef enum { WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
typedef int EventGroupHandle_t;
typedef int SemaphoreHandle_t;
typedef int esp_timer_handle_t;

#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#define WIFI_STORAGE_RAM 0
#define CONNECTED_BIT (1u << 0)
#define FAILED_BIT (1u << 1)
#define WIFI_SECOND_CHAN_NONE 0
#define OFF_DELAY_US (8 * 1000 * 1000)
#define WIFI_AUTH_WPA2_PSK 1
#define WIFI_AUTH_OPEN 0
#define WPA3_SAE_PWE_BOTH 1
#define WIFI_IF_STA 0
#define WIFI_SCAN_MAX 20
#define ESP_LOGW(...) ((void)0)
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define STATE_WIFI 1
#define STATE_WIFI_OFF 0
#define STATE_WIFI_AP 2
#define STATE_WIFI_CONNECTING 3
#define pdTRUE 1
#define portMAX_DELAY 0x7fffffff
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)

static const char *TAG = "wifi-test";
static esp_netif_t *s_sta, *s_ap;
static bool s_driver, s_driver_ready, s_started, s_sta_wanted, s_ap_on;
static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_scan_mutex;
static esp_timer_handle_t s_off_timer;
static int s_refs;
static unsigned s_radio_refs, s_scans;
static int s_retries;
static wifi_channel_policy_t s_channel_policy;
static const uint8_t s_intercom_owner[4] = {'O', 'A', '0', '1'};
static uint8_t s_common_channel;
static unsigned event_bits;
static uint8_t sdk_channel;
static int disconnect_calls, get_channel_calls, set_channel_calls;
static int wifi_connect_calls, wifi_set_config_calls, scan_start_calls;
static int join_best_calls, ap_client_checks;
static int disconnect_result, get_channel_result, set_channel_result;
static int wifi_connect_result, wifi_set_config_result, scan_start_result;
static int nvs_open_calls, nvs_set_u8_calls, nvs_commit_calls, nvs_get_u8_calls, nvs_close_calls;
static int nvs_open_result, nvs_set_u8_result, nvs_commit_result, nvs_get_u8_result;
static uint8_t nvs_channel;
static bool mutex_busy;
static int last_mutex_wait;
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NVS_NAMESPACE "wifi"
#define NVS_KEY "known"
#define WIFI_KNOWN_MAX 8
typedef struct { char ssid[33], password[65]; } wifi_known_t;
static wifi_known_t s_known[WIFI_KNOWN_MAX];
static int s_known_count;
static char s_preferred[33];

static int create_sta_calls, create_ap_calls, destroy_calls;
static int init_calls, deinit_calls, storage_calls, mode_calls, start_calls, stop_calls;
static int clear_bits_calls, state_calls, timer_calls, post_calls, mutex_take_calls, mutex_give_calls;
static uint64_t last_timer_delay;
static int init_result, deinit_result, storage_result, mode_result, start_result, stop_result;
static bool create_sta_fail, create_ap_fail, post_result = true;
static unsigned last_cleared_bits, last_state_value;

static esp_netif_t sta_netif = {.slot = 1}, ap_netif = {.slot = 2};

static esp_netif_t *esp_netif_create_default_wifi_sta(void)
{ create_sta_calls++; return create_sta_fail ? NULL : &sta_netif; }
static esp_netif_t *esp_netif_create_default_wifi_ap(void)
{ create_ap_calls++; return create_ap_fail ? NULL : &ap_netif; }
static void esp_netif_destroy_default_wifi(esp_netif_t *netif)
{ (void)netif; destroy_calls++; }
static esp_err_t esp_wifi_init(const wifi_init_config_t *cfg)
{ (void)cfg; init_calls++; return init_result; }
static esp_err_t esp_wifi_deinit(void)
{ deinit_calls++; return deinit_result; }
static esp_err_t esp_wifi_set_storage(int storage)
{ (void)storage; storage_calls++; return storage_result; }
static esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{ (void)mode; mode_calls++; return mode_result; }
static esp_err_t esp_wifi_start(void)
{ start_calls++; return start_result; }
static esp_err_t esp_wifi_stop(void)
{ stop_calls++; return stop_result; }
static esp_err_t esp_wifi_disconnect(void)
{ disconnect_calls++; return disconnect_result; }
static esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config)
{ (void)interface; (void)config; wifi_set_config_calls++; return wifi_set_config_result; }
static esp_err_t esp_wifi_connect(void)
{ wifi_connect_calls++; return wifi_connect_result; }
static esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{ (void)config; (void)block; scan_start_calls++; return scan_start_result; }
static esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count, wifi_ap_record_t *records)
{ (void)records; *count = 0; return ESP_OK; }
static void esp_wifi_clear_ap_list(void) {}
static esp_err_t esp_wifi_get_channel(uint8_t *primary, wifi_second_chan_t *second)
{ (void)second; get_channel_calls++; if (get_channel_result == ESP_OK) *primary = sdk_channel; return get_channel_result; }
static esp_err_t esp_wifi_set_channel(uint8_t channel, wifi_second_chan_t second)
{ (void)second; set_channel_calls++; if (set_channel_result == ESP_OK) sdk_channel = channel; return set_channel_result; }
static const char *esp_err_to_name(esp_err_t err)
{ (void)err; return "stub"; }
static void sys_heap_log(const char *label) { (void)label; }
static void apply_power_save(void) {}
static void ap_set(bool on) { s_ap_on = on; }
static unsigned xEventGroupClearBits(EventGroupHandle_t group, unsigned bits)
{ (void)group; clear_bits_calls++; last_cleared_bits = bits; unsigned before = event_bits; event_bits &= ~bits; return before; }
static unsigned xEventGroupGetBits(EventGroupHandle_t group)
{ (void)group; return event_bits; }
static EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, unsigned bits,
                                       int clear, int all, int ticks)
{ (void)group; (void)bits; (void)clear; (void)all; (void)ticks; return event_bits; }
static int xSemaphoreTake(SemaphoreHandle_t mutex, int wait)
{ (void)mutex; mutex_take_calls++; last_mutex_wait = wait; return mutex_busy && wait == 0 ? 0 : pdTRUE; }
static int xSemaphoreGive(SemaphoreHandle_t mutex)
{ (void)mutex; mutex_give_calls++; return pdTRUE; }
static esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{ (void)timer; last_timer_delay = timeout_us; timer_calls++; return ESP_OK; }
static esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{ (void)timer; return ESP_OK; }
static bool sys_post(void (*fn)(void *), void *arg)
{ (void)fn; (void)arg; post_calls++; return post_result; }
static void state_set(int key, unsigned value)
{ (void)key; state_calls++; last_state_value = value; }
static bool wifi_is_connected(void)
{ return (event_bits & CONNECTED_BIT) != 0; }
static bool ap_has_clients(void)
{ ap_client_checks++; return false; }
static esp_err_t join_best_known(uint32_t timeout_ms)
{ (void)timeout_ms; join_best_calls++; return ESP_OK; }
static esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ (void)name; (void)mode; nvs_open_calls++; if (nvs_open_result == ESP_OK) *handle = 1; return nvs_open_result; }
static esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{ (void)handle; (void)key; nvs_get_u8_calls++; if (nvs_get_u8_result == ESP_OK) *value = nvs_channel; return nvs_get_u8_result; }
static esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size)
{ (void)handle; (void)key; (void)value; (void)size; return ESP_FAIL; }
static esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{ (void)handle; (void)key; (void)value; (void)size; return ESP_FAIL; }
static esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{ (void)handle; (void)key; nvs_set_u8_calls++; if (nvs_set_u8_result == ESP_OK) nvs_channel = value; return nvs_set_u8_result; }
static esp_err_t nvs_commit(nvs_handle_t handle)
{ (void)handle; nvs_commit_calls++; return nvs_commit_result; }
static void nvs_close(nvs_handle_t handle)
{ (void)handle; nvs_close_calls++; }

static wifi_mode_t wanted_mode(bool need_sta) { (void)need_sta; return WIFI_MODE_STA; }

static void reset_wifi_stubs(void)
{
    s_sta = s_ap = NULL;
    s_driver = s_driver_ready = s_started = s_sta_wanted = s_ap_on = false;
    s_events = 7; s_mutex = 8; s_scan_mutex = 10; s_off_timer = 9; s_refs = 0;
    s_radio_refs = s_scans = 0;
    s_retries = 0;
    memset(&s_channel_policy, 0, sizeof(s_channel_policy));
    s_common_channel = 1;
    s_known_count = 0; s_preferred[0] = '\0';
    event_bits = 0; sdk_channel = 1;
    disconnect_calls = get_channel_calls = set_channel_calls = 0;
    wifi_connect_calls = wifi_set_config_calls = scan_start_calls = 0;
    join_best_calls = ap_client_checks = 0;
    disconnect_result = get_channel_result = set_channel_result = ESP_OK;
    wifi_connect_result = wifi_set_config_result = scan_start_result = ESP_OK;
    nvs_open_calls = nvs_set_u8_calls = nvs_commit_calls = nvs_get_u8_calls = nvs_close_calls = 0;
    nvs_open_result = nvs_set_u8_result = nvs_commit_result = ESP_OK;
    nvs_get_u8_result = ESP_FAIL; nvs_channel = 1;
    mutex_busy = false; last_mutex_wait = 0;
    create_sta_calls = create_ap_calls = destroy_calls = 0;
    init_calls = deinit_calls = storage_calls = mode_calls = start_calls = stop_calls = 0;
    clear_bits_calls = state_calls = timer_calls = post_calls = mutex_take_calls = mutex_give_calls = 0;
    last_timer_delay = 0;
    init_result = deinit_result = storage_result = mode_result = start_result = stop_result = ESP_OK;
    create_sta_fail = create_ap_fail = false;
    post_result = true;
    last_cleared_bits = last_state_value = 0;
}

#endif
