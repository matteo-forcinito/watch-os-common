#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_ARG 3
#define ERR_OK 0
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
typedef struct { int locked; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static int critical_depth, mutex_creates;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) do { (void)(m); critical_depth++; } while (0)
#define portEXIT_CRITICAL(m) do { (void)(m); assert(critical_depth > 0); critical_depth--; } while (0)
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *m) { assert(critical_depth > 0); mutex_creates++; return m; }
static int xSemaphoreTake(SemaphoreHandle_t m, uint32_t wait) { (void)wait; if(m->locked) return 0; m->locked=1; return pdTRUE; }
static void xSemaphoreGive(SemaphoreHandle_t m) { assert(m->locked); m->locked=0; }

typedef struct { int nvs_enable; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef enum { WIFI_STORAGE_RAM } wifi_storage_t;
typedef enum { WIFI_MODE_STA } wifi_mode_t;
typedef enum { WIFI_PS_NONE } wifi_ps_type_t;
typedef enum { WIFI_SECOND_CHAN_NONE } wifi_second_chan_t;
static int fail_event, fail_init, fail_storage, fail_mode, fail_start, fail_ps;
static int fail_stop, fail_deinit, fail_channel;
static int call_event, call_init, call_storage, call_mode, call_start, call_ps, call_stop, call_deinit, call_channel, call_get_channel;
static uint8_t last_channel;
static esp_err_t esp_event_loop_create_default(void) { call_event++; return fail_event; }
static esp_err_t esp_wifi_init(const wifi_init_config_t *c) { assert(!c->nvs_enable); call_init++; return fail_init; }
static esp_err_t esp_wifi_set_storage(wifi_storage_t s) { (void)s; call_storage++; return fail_storage; }
static esp_err_t esp_wifi_set_mode(wifi_mode_t m) { (void)m; call_mode++; return fail_mode; }
static esp_err_t esp_wifi_start(void) { call_start++; return fail_start; }
static esp_err_t esp_wifi_set_ps(wifi_ps_type_t p) { (void)p; call_ps++; return fail_ps; }
static esp_err_t esp_wifi_stop(void) { call_stop++; return fail_stop; }
static esp_err_t esp_wifi_deinit(void) { call_deinit++; return fail_deinit; }
static esp_err_t esp_wifi_set_channel(uint8_t c, wifi_second_chan_t s) { (void)s; call_channel++; last_channel=c; return fail_channel; }
static esp_err_t esp_wifi_get_channel(uint8_t *p, wifi_second_chan_t *s) { call_get_channel++; *p=last_channel; *s=WIFI_SECOND_CHAN_NONE; return ESP_OK; }

typedef void (*esp_timer_cb_t)(void *);
typedef void *esp_timer_handle_t;
typedef struct { esp_timer_cb_t callback; const char *name; } esp_timer_create_args_t;
static esp_timer_cb_t timer_cb;
static int timer_starts, timer_creates;
static esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) { timer_cb=a->callback; *h=(void*)1; timer_creates++; return ESP_OK; }
static esp_err_t esp_timer_stop(esp_timer_handle_t h) { (void)h; return ESP_OK; }
static esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us) { (void)h; assert(us==100000); timer_starts++; return ESP_OK; }
typedef void (*sys_fn_t)(void *);
static sys_fn_t posted_fn;
static int fail_post;
static bool sys_post(sys_fn_t fn, void *arg) { if(fail_post) return false; assert(arg==NULL); posted_fn=fn; return true; }

static void reset_mocks(void)
{
    fail_event=fail_init=fail_storage=fail_mode=fail_start=fail_ps=0;
    fail_stop=fail_deinit=fail_channel=0;
    call_event=call_init=call_storage=call_mode=call_start=call_ps=call_stop=call_deinit=call_channel=call_get_channel=0;
    timer_starts=0; timer_creates=0; posted_fn=NULL; fail_post=0; last_channel=0;
}
