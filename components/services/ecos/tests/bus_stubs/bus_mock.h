#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_NOT_FOUND 4
#define ESP_ERR_ESPNOW_EXIST 5
#define WIFI_IF_STA 0
#define ESP_NOW_MAX_DATA_LEN_V2 1470
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef struct { const uint8_t *src_addr; } esp_now_recv_info_t;
typedef struct { const uint8_t *des_addr; } esp_now_send_info_t;
typedef int esp_now_send_status_t;
typedef struct { int channel, ifidx; bool encrypt; uint8_t peer_addr[6]; } esp_now_peer_info_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef int StaticSemaphore_t;
typedef int *SemaphoreHandle_t;
#define portMAX_DELAY 100
#define pdTRUE 1
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { return s; }
static int lock_depth;
static void (*send_hook)(void);
static int xSemaphoreTake(SemaphoreHandle_t s, int t) { (void)s; (void)t; if (lock_depth) return 0; lock_depth=1; return pdTRUE; }
static void xSemaphoreGive(SemaphoreHandle_t s) { (void)s; assert(lock_depth==1); lock_depth=0; }
static int leases, inits, deinits, power = 80, fail_init, fail_recv, fail_sent;
static int recv_count, sent_count, fail_power, fail_send, send_count;
static uint8_t last_send_mac[6];
static void (*radio_recv)(const esp_now_recv_info_t *, const uint8_t *, int);
static void (*radio_sent)(const esp_now_send_info_t *, esp_now_send_status_t);
static esp_err_t wifi_radio_acquire(void) { leases++; return ESP_OK; }
static void wifi_radio_release(void) { leases--; }
static esp_err_t esp_now_init(void) { inits++; return fail_init; }
static void esp_now_deinit(void) { deinits++; }
static esp_err_t esp_now_register_recv_cb(void (*cb)(const esp_now_recv_info_t *, const uint8_t *, int)) { radio_recv=cb; return fail_recv; }
static esp_err_t esp_now_register_send_cb(void (*cb)(const esp_now_send_info_t *, esp_now_send_status_t)) { radio_sent=cb; return fail_sent; }
static void esp_now_unregister_recv_cb(void) { radio_recv=NULL; }
static void esp_now_unregister_send_cb(void) { radio_sent=NULL; }
static esp_err_t esp_wifi_get_max_tx_power(int8_t *p) { *p=(int8_t)power; return ESP_OK; }
static esp_err_t esp_wifi_set_max_tx_power(int8_t p) { if (fail_power) return fail_power; power=p; return ESP_OK; }
static bool esp_now_is_peer_exist(const uint8_t *m) { return m[0]==1; }
static esp_err_t esp_now_add_peer(const esp_now_peer_info_t *p) { return p->channel==0 && !p->encrypt ? ESP_OK : 1; }
static esp_err_t esp_now_send(const uint8_t *m, const uint8_t *d, size_t n) { (void)d; assert(lock_depth==1); memcpy(last_send_mac,m,6); send_count++; if(send_hook) send_hook(); return fail_send; }
static void vTaskDelay(int ticks);
