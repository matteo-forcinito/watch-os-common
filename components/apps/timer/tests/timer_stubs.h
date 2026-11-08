#ifndef TIMER_REMOTE_TEST_STUBS_H
#define TIMER_REMOTE_TEST_STUBS_H

#include <stdbool.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103

typedef struct stub_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *);
typedef struct { esp_timer_cb_t callback; const char *name; } esp_timer_create_args_t;

typedef struct { bool running; int64_t started_us, accumulated_us; } stopwatch_t;
typedef struct { int unused; } lv_event_t;
typedef struct { int unused; } lv_obj_t;
struct stub_timer { int unused; };

extern struct stub_timer timer_instance;
extern stopwatch_t s_watch;
extern esp_timer_handle_t s_countdown;
extern int64_t s_countdown_end_us;
extern int s_countdown_minutes;
extern int64_t s_countdown_duration_us;
extern lv_obj_t *s_watch_button;

int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
void sound_alarm_stop(void);
lv_obj_t *lv_obj_get_child(lv_obj_t *obj, int index);
void lv_label_set_text(lv_obj_t *obj, const char *text);

#endif
