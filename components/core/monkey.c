/*
 * Monkey test (CONFIG_WATCH_MONKEY_TEST, off in normal builds).
 *
 * Plays with the watch like a restless user, for hours, to find crashes,
 * freezes and leaks before a real user does:
 *
 *   swipes (scroll, pages), the "back" edge swipe, apps opened and closed,
 *   home, screen off/on (button and tap-to-wake), always-on face...
 *
 * It never taps: a random tap could power the watch off, format the SD card,
 * start the USB drive (which takes the USB port and the log with it) or start
 * a countdown that rings forever. Every swipe moves at least MIN_SWIPE_PX, or
 * LVGL would take it for a tap. Only apps in s_safe_apps are opened: none of
 * them can make a sound or take the USB port. A health report is logged every minute.
 */

#include "core/monkey.h"

#include "core/app.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/sys.h"

#include "hardware/display.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "monkey";

#if CONFIG_WATCH_MONKEY_TEST

#define TASK_STACK        4096
#define HEALTH_EVERY_US   (60LL * 1000000)
#define SWIPE_STEP_MS     16
#define MIN_SWIPE_PX      80    /* shorter = a tap for LVGL */

static const char *const s_safe_apps[] = {
    "weather", "battery", "settings", "settings.display", "settings.wifi",
    "settings.bluetooth", "settings.time", "settings.about", "launcher.all",
};

#define SAFE_APP_COUNT (sizeof(s_safe_apps) / sizeof(s_safe_apps[0]))

static uint32_t s_actions = 0;

static int rand_range(int lo, int hi)
{
    return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1));
}

static void pause_ms(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* A finger moving from (x1,y1) to (x2,y2) in `steps` reads, then lifting. */
static int clamp(int v, int max)
{
    return v < 0 ? 0 : (v > max ? max : v);
}

static void swipe(int x1, int y1, int x2, int y2, int steps)
{
    x2 = clamp(x2, display_width() - 1);
    y2 = clamp(y2, display_height() - 1);

    for (int i = 0; i <= steps; i++)
    {
        lv_port_fake_touch(true, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
        pause_ms(SWIPE_STEP_MS);
    }

    lv_port_fake_touch(false, x2, y2);
}

static void open_app_cb(void *arg)
{
    app_open((const char *)arg, NULL);
}

static void home_cb(void *arg)
{
    (void)arg;
    app_home();
}

static void back_cb(void *arg)
{
    (void)arg;
    app_back();
}

static void wait_screen(power_screen_t want, int timeout_ms)
{
    for (int t = 0; t < timeout_ms && power_screen() != want; t += 50)
    {
        pause_ms(50);
    }
}

static void do_action(void)
{
    const int w = display_width();
    const int h = display_height();
    int pick = rand_range(0, 99);

    if (power_screen() != POWER_SCREEN_ON)
    {
        power_wake(POWER_WAKE_BUTTON);
        wait_screen(POWER_SCREEN_ON, 2000);
    }

    if (pick < 25)
    {
        /* Scroll a list: up or down, slow or flicked. */
        int x = rand_range(w / 4, w * 3 / 4);
        int y1 = rand_range(h / 4, h * 3 / 4);
        int dy = rand_range(MIN_SWIPE_PX, h / 2) * (rand_range(0, 1) ? 1 : -1);
        swipe(x, y1, x, y1 + dy, rand_range(3, 15));
    }
    else if (pick < 37)
    {
        /* Change page (home pager, tile views), never from the left edge. */
        int y = rand_range(h / 4, h * 3 / 4);
        bool left = rand_range(0, 1);
        swipe(left ? w * 4 / 5 : w / 3, y, left ? w / 5 : w * 4 / 5, y, rand_range(4, 10));
    }
    else if (pick < 49)
    {
        /* "Back": swipe right from the left edge. */
        int y = rand_range(h / 4, h * 3 / 4);
        swipe(4, y, w / 2, y, 8);
    }
    else if (pick < 67)
    {
        const char *id = s_safe_apps[rand_range(0, SAFE_APP_COUNT - 1)];
        ESP_LOGI(TAG, "#%u open %s", (unsigned)s_actions, id);
        ui_async(open_app_cb, (void *)id);
        pause_ms(rand_range(200, 1500));
    }
    else if (pick < 73)
    {
        ui_async(rand_range(0, 1) ? home_cb : back_cb, NULL);
    }
    else if (pick < 85)
    {
        /* Screen off, then woken by the button or by a tap (tap to wake). */
        ESP_LOGI(TAG, "#%u sleep/wake", (unsigned)s_actions);
        power_sleep_now();
        pause_ms(rand_range(50, 4000));

        if (rand_range(0, 1) || power_screen() == POWER_SCREEN_ON || power_screen() == POWER_SCREEN_DIM)
        {
            power_wake(POWER_WAKE_BUTTON);   /* a tap on a lit screen would press something */
        }
        else
        {
            lv_port_fake_touch(true, w / 2, h / 2);
            pause_ms(60);
            lv_port_fake_touch(false, w / 2, h / 2);
        }

        wait_screen(POWER_SCREEN_ON, 3000);
    }
    else if (pick < 90)
    {
        /* Wake in the middle of the fade out / sleep sequence. */
        power_sleep_now();
        pause_ms(rand_range(0, 150));
        power_wake(POWER_WAKE_BUTTON);
    }
    else
    {
        pause_ms(rand_range(300, 3000));   /* idle: animations finish, timers run */
    }

    pause_ms(rand_range(20, 400));
}

static void monkey_task(void *arg)
{
    (void)arg;

    pause_ms(10000);   /* let the boot finish */
    ESP_LOGW(TAG, "Monkey test running");

    int64_t next_health = esp_timer_get_time() + HEALTH_EVERY_US;

    while (true)
    {
        do_action();
        s_actions++;

        if (esp_timer_get_time() >= next_health)
        {
            ESP_LOGI(TAG, "%u actions so far", (unsigned)s_actions);
            sys_health_log();
            next_health = esp_timer_get_time() + HEALTH_EVERY_US;
        }
    }
}

void monkey_start(void)
{
    sys_task_create(monkey_task, "monkey", TASK_STACK, NULL, 2, SYS_CORE_ANY);
}

bool monkey_app_allowed(const char *id)
{
    for (size_t i = 0; i < SAFE_APP_COUNT; i++)
    {
        if (strcmp(s_safe_apps[i], id) == 0)
        {
            return true;
        }
    }

    ESP_LOGW(TAG, "Not opening \"%s\" (not safe for the monkey test)", id);

    return false;
}

#else

void monkey_start(void)
{
    (void)TAG;
}

bool monkey_app_allowed(const char *id)
{
    (void)id;
    return true;
}

#endif
