#ifndef WATCH_LITE_H
#define WATCH_LITE_H
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "services/alarm.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct
{
    uint32_t version;
    bool aod;
    uint8_t brightness;
    uint16_t web_minutes;
} lite_settings_t;
typedef struct
{
    int kind, value, second;
} lite_event_t;
enum
{
    LITE_WAKE = 1,
    LITE_BUTTON,
    LITE_TOUCH,
    LITE_KNOB,
    LITE_DIRTY,
    LITE_WEB,
    LITE_EXIT
};
extern QueueHandle_t lite_events;
extern lite_settings_t lite_settings;
void lite_post(int kind, int value, int second);
void lite_runtime_init(void);
void lite_save(void);
void lite_ring(const alarm_t *alarm);
void lite_alert_stop(bool snooze);
bool lite_ringing(void);
void lite_alert_text(char *out, unsigned size);
bool lite_draw_init(void);
uint32_t lite_draw_frames(void);
void lite_draw(bool menu, int selected, bool idle, const char *address, const char *code);
bool lite_web_running(void);
void lite_web_set(bool enabled);
void lite_web_address(char *out, unsigned size);
const char *lite_web_code(void);
void lite_web_tick(void);
void lite_return(void);
#endif
