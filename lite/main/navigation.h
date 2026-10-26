#ifndef LITE_NAVIGATION_H
#define LITE_NAVIGATION_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { LITE_CLOCK, LITE_SETTINGS, LITE_CONNECTION, LITE_CONFIRM_EXIT,
    LITE_APPS, LITE_ALARMS, LITE_AGENDA, LITE_COLLECTION, LITE_DOGS, LITE_PREFERENCES } lite_page_t;
typedef enum {
    LITE_NAV_NONE, LITE_NAV_CHANGED, LITE_NAV_WAKE, LITE_NAV_AOD,
    LITE_NAV_WIFI, LITE_NAV_BRIGHTNESS, LITE_NAV_EXIT, LITE_NAV_SLEEP,
    LITE_NAV_STOP, LITE_NAV_SNOOZE, LITE_NAV_IDLE, LITE_NAV_PRIMARY,
    LITE_NAV_TAP_WAKE, LITE_NAV_TIME_FORMAT, LITE_NAV_VOLUME, LITE_NAV_TIMEOUT
} lite_action_t;
typedef struct {
    lite_page_t page;
    int selected, width, height, offset;
    bool down, wake_only, cancelled;
    int x, y, last_x, last_y, travel_x, travel_y;
    int64_t began, released, exit_at;
} lite_navigation_t;
void lite_navigation_init(lite_navigation_t *nav, int width, int height);
int lite_navigation_row_height(const lite_navigation_t *nav);
int lite_navigation_row_top(const lite_navigation_t *nav);
int lite_navigation_rows(const lite_navigation_t *nav);
lite_action_t lite_navigation_touch(lite_navigation_t *nav, bool pressed, int x, int y,
                                    int64_t now, bool idle, bool alert);
/* Logical keys match hardware/button.h; no driver/RTOS dependency here. */
lite_action_t lite_navigation_key(lite_navigation_t *nav, bool power, int event,
                                  int64_t now, bool idle, bool alert, bool knob);
lite_action_t lite_navigation_knob(lite_navigation_t *nav, int delta);
#endif
