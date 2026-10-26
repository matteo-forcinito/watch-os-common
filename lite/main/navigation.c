#include "navigation.h"
#include <stdlib.h>
#include <string.h>

void lite_navigation_init(lite_navigation_t *n, int w, int h)
{
    memset(n, 0, sizeof(*n));
    n->width = w;
    n->height = h;
}
int lite_navigation_rows(const lite_navigation_t *n) { return n->page == LITE_SETTINGS ? 5 : 4; }
int lite_navigation_row_height(const lite_navigation_t *n) { return n->height >= 360 ? 44 : 34; }
int lite_navigation_row_top(const lite_navigation_t *n) { return (n->height - lite_navigation_rows(n) * lite_navigation_row_height(n)) / 2; }
static lite_action_t page(lite_navigation_t *n, lite_page_t p, int64_t now)
{
    n->page = p;
    n->offset = 0;
    n->selected = 0;
    if (p == LITE_CONFIRM_EXIT) n->exit_at = now;
    return LITE_NAV_CHANGED;
}
static lite_action_t select_row(lite_navigation_t *n, int row, int64_t now)
{
    n->selected = row;
    if (n->page == LITE_APPS) {
        static const lite_page_t apps[] = {LITE_ALARMS,LITE_AGENDA,LITE_COLLECTION,LITE_DOGS};
        return row >= 0 && row < 4 ? page(n, apps[row], now) : LITE_NAV_NONE;
    }
    if (n->page == LITE_PREFERENCES) {
        static const lite_action_t actions[] = {LITE_NAV_TAP_WAKE,LITE_NAV_TIME_FORMAT,LITE_NAV_VOLUME,LITE_NAV_TIMEOUT};
        return row >= 0 && row < 4 ? actions[row] : LITE_NAV_NONE;
    }
    switch (row) {
    case 0: return LITE_NAV_AOD;
    case 1: return LITE_NAV_WIFI;
    case 2: return LITE_NAV_BRIGHTNESS;
    case 3: return page(n, LITE_PREFERENCES, now);
    case 4: return page(n, LITE_CONFIRM_EXIT, now);
    default: return LITE_NAV_NONE;
    }
}
lite_action_t lite_navigation_touch(lite_navigation_t *n, bool pressed, int x, int y,
                                    int64_t now, bool idle, bool alert)
{
    if (pressed) {
        if (x < 0 || y < 0 || x >= n->width || y >= n->height) {
            if (n->down) n->cancelled = true;
            return LITE_NAV_NONE;
        }
        if (!n->down) {
            n->down = true;
            n->wake_only = idle;
            n->cancelled = false;
            n->x = n->last_x = x;
            n->y = n->last_y = y;
            n->travel_x = n->travel_y = 0;
            n->began = now;
            n->released = 0;
            return idle ? LITE_NAV_WAKE : LITE_NAV_NONE;
        }
        n->released = 0;
        n->last_x = x;
        n->last_y = y;
        if (abs(x - n->x) > n->travel_x) n->travel_x = abs(x - n->x);
        if (abs(y - n->y) > n->travel_y) n->travel_y = abs(y - n->y);
        return LITE_NAV_NONE;
    }
    if (!n->down) return LITE_NAV_NONE;
    if (!n->released) { n->released = now; return LITE_NAV_NONE; }
    if (now - n->released < 24000) return LITE_NAV_NONE;
    n->down = false;
    if (n->wake_only || n->cancelled) return LITE_NAV_NONE;
    int64_t duration = n->released - n->began;
    if (duration < 40000 || duration > 2000000) return LITE_NAV_NONE;
    int dx = n->last_x - n->x, dy = n->last_y - n->y;
    int horizontal = n->width / 7, vertical = n->height / 7;
    if (horizontal < 36) horizontal = 36;
    if (vertical < 36) vertical = 36;
    bool swipe_x = abs(dx) >= horizontal && abs(dx) * 2 > abs(dy) * 3;
    bool swipe_y = abs(dy) >= vertical && abs(dy) * 2 > abs(dx) * 3;
    if (alert) {
        if (n->travel_x <= 12 && n->travel_y <= 12 && duration <= 700000)
            return n->x < n->width / 3 ? LITE_NAV_SNOOZE : LITE_NAV_STOP;
        return LITE_NAV_NONE;
    }
    if (n->page == LITE_CONFIRM_EXIT) {
        /* A new full-width deliberate gesture. Taps, jitter, a held finger,
         * and the gesture that opened this page can never restart the device. */
        if (swipe_x && dx >= n->width / 2 && n->x <= n->width / 3 &&
            n->last_x >= n->width * 2 / 3 && abs(dy) <= n->height / 5 &&
            duration >= 180000 && duration <= 1500000)
            return LITE_NAV_EXIT;
        if (swipe_y && dy > 0) return page(n, LITE_SETTINGS, now);
        return LITE_NAV_NONE;
    }
    if (swipe_x || swipe_y) {
        if (n->page == LITE_CLOCK)
            return page(n, swipe_x && dx < 0 ? LITE_APPS : swipe_y && dy > 0 ? LITE_CONNECTION : LITE_SETTINGS, now);
        if (n->page >= LITE_ALARMS && n->page <= LITE_DOGS && swipe_y) {
            n->offset += dy < 0 ? 1 : -1;
            if (n->offset < 0) n->offset = 0;
            if (n->offset > 1023) n->offset = 0;
            return LITE_NAV_CHANGED;
        }
        return page(n, LITE_CLOCK, now);
    }
    if (n->travel_x > 12 || n->travel_y > 12 || duration > 700000) return LITE_NAV_NONE;
    if ((n->page == LITE_SETTINGS || n->page == LITE_APPS || n->page == LITE_PREFERENCES) &&
        n->x >= n->width / 8 && n->x < n->width * 7 / 8) {
        int top = lite_navigation_row_top(n), height = lite_navigation_row_height(n);
        if (n->y >= top && n->y < top + height * lite_navigation_rows(n)) {
            int row = (n->y - top) / height;
            if (n->last_y >= top + row * height && n->last_y < top + (row + 1) * height - 4 &&
                n->y < top + (row + 1) * height - 4)
                return select_row(n, row, now);
        }
    }
    if ((n->page == LITE_CONNECTION || n->page == LITE_ALARMS || n->page == LITE_DOGS) &&
        n->x >= n->width / 5 && n->x < n->width * 4 / 5 &&
        n->y >= n->height / 2 + 40 && n->y < n->height / 2 + 80)
        return n->page == LITE_CONNECTION ? LITE_NAV_WIFI : LITE_NAV_PRIMARY;
    return LITE_NAV_NONE;
}
lite_action_t lite_navigation_key(lite_navigation_t *n, bool power, int event,
                                  int64_t now, bool idle, bool alert, bool knob)
{
    if (event == 2 && knob) return idle ? LITE_NAV_WAKE : page(n, LITE_CLOCK, now);
    if (event != 0 && event != 1) return LITE_NAV_NONE; /* click / long press */
    if (idle) return LITE_NAV_WAKE;
    if (alert && event == 0) return power ? LITE_NAV_SNOOZE : LITE_NAV_STOP;
    if (power) {
        if (event == 0) return LITE_NAV_SLEEP;
        return page(n, LITE_SETTINGS, now);
    }
    if (event == 1) {
        if (n->page == LITE_CONFIRM_EXIT && now - n->exit_at >= 1200000) return LITE_NAV_EXIT;
        return page(n, LITE_CONFIRM_EXIT, now);
    }
    if (n->page == LITE_CONFIRM_EXIT) return page(n, LITE_SETTINGS, now);
    if (knob && (n->page == LITE_SETTINGS || n->page == LITE_APPS || n->page == LITE_PREFERENCES))
        return select_row(n, n->selected, now);
    if (knob && (n->page == LITE_ALARMS || n->page == LITE_DOGS)) return LITE_NAV_PRIMARY;
    if (n->page == LITE_CLOCK) return knob ? page(n, LITE_APPS, now) : LITE_NAV_IDLE;
    return page(n, LITE_CLOCK, now);
}
lite_action_t lite_navigation_knob(lite_navigation_t *n, int delta)
{
    if (n->page >= LITE_ALARMS && n->page <= LITE_DOGS) {
        n->offset = (n->offset + delta + 1024) % 1024;
        return LITE_NAV_CHANGED;
    }
    if (n->page != LITE_SETTINGS && n->page != LITE_PREFERENCES && n->page != LITE_APPS) n->page = LITE_APPS;
    int count = lite_navigation_rows(n);
    n->selected = (n->selected + delta % count + count) % count;
    return LITE_NAV_CHANGED;
}
