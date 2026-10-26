#include "core/power.h"
#include "core/clock.h"

#include "core/lv_port.h"
#include "core/settings.h"
#include "hardware/board.h"
#include "core/recovery.h"
#include "core/state.h"
#include "core/sys.h"

#include "hardware/display.h"
#include "hardware/pmu.h"
#include "hardware/touch.h"
#include "hardware/usb_console.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdatomic.h>

static const char *TAG = "power";

/* I2C (PMU, touch), panel commands, LVGL lock and logs: 4 KB overflowed once. */
#define TASK_STACK          6144
#define TASK_PRIORITY       5
#define DIM_BEFORE_OFF_MS   4000
#define AOD_BRIGHTNESS      24
#define KEY_POLL_ON_MS      150
#define KEY_POLL_OFF_MS     300
#define BATTERY_POLL_ON_MS  20000
#define BATTERY_POLL_OFF_MS 120000
#define FADE_STEPS          6
#define FADE_STEP_MS        20
#define LVGL_STALL_MS       8000   /* one lvgl pass this long = the UI is stuck */

typedef enum
{
    MSG_BUTTON,
    MSG_TOUCH_WAKE,
    MSG_WAKE,
    MSG_SLEEP,
    MSG_SETTINGS,
    MSG_MIRROR,
} msg_type_t;

typedef struct
{
    msg_type_t type;
    int a;
    int b;
} msg_t;

static QueueHandle_t s_queue = NULL;
static _Atomic int64_t s_last_activity_us = 0;
static _Atomic int s_keep_on = 0;
static power_screen_t s_screen = POWER_SCREEN_ON;
static uint8_t s_brightness_now = 0;
static void (*s_button_handler)(button_id_t, button_event_t) = NULL;
static esp_pm_lock_handle_t s_cpu_lock = NULL;
static esp_pm_lock_handle_t s_usb_lock = NULL;   /* no light sleep while the cable has power */
static bool s_usb_power = false;
static bool s_inputs_started;
static _Atomic int s_cpu_boost = 0;
static bool s_mirror = false;   /* remote mirror: the panel sleeps, the UI keeps running (power task) */

static uint8_t active_brightness(void)
{
    uint8_t value = settings_get()->brightness;
    return settings_get()->energy_saver && value > 80 ? 80 : value;
}
static uint8_t aod_brightness(void)
{
    uint8_t requested = settings_get()->energy_saver ? 8 : AOD_BRIGHTNESS;
    return requested < board_info()->min_aod_brightness ? board_info()->min_aod_brightness : requested;
}

/* ------------------------------------------------------------ helpers */

/* The physical panel and touch are in use (not headless, not mirrored). */
static bool panel_used(void)
{
    return !lv_port_headless() && !s_mirror;
}

static void post(msg_type_t type, int a, int b)
{
    msg_t msg = {.type = type, .a = a, .b = b};

    if (s_queue != NULL)
    {
        xQueueSend(s_queue, &msg, 0);
    }
}

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void set_brightness(uint8_t target, bool fade)
{
    if (!fade)
    {
        if (panel_used()) display_set_brightness(target);
        s_brightness_now = target;
        return;
    }

    int from = s_brightness_now;

    for (int i = 1; i <= FADE_STEPS; i++)
    {
        int level = from + ((int)target - from) * i / FADE_STEPS;
        if (panel_used()) display_set_brightness((uint8_t)level);
        vTaskDelay(pdMS_TO_TICKS(FADE_STEP_MS));
    }

    s_brightness_now = target;
}

static void set_screen(power_screen_t screen)
{
    s_screen = screen;
    state_set(STATE_SCREEN, screen);
}

/* ------------------------------------------------------ state changes */

static void go_on(void)
{
    power_screen_t from = s_screen;

    if (from == POWER_SCREEN_ON)
    {
        return;
    }

    if (from == POWER_SCREEN_OFF)
    {
        if (panel_used()) display_sleep(false);
    }

    if (panel_used()) touch_set_low_power(false);
    set_screen(POWER_SCREEN_ON);   /* the app manager puts the normal screen back */
    lv_port_set_running(true);

    /* Let the UI swap screens, then draw the first frame before lighting up. */
    vTaskDelay(pdMS_TO_TICKS(20));
    lv_port_refresh_now();

    set_brightness(active_brightness(), true);
    atomic_store(&s_last_activity_us, now_us());

    ESP_LOGI(TAG, "Screen on");
}

static void go_dim(void)
{
    if (s_screen != POWER_SCREEN_ON)
    {
        return;
    }

    uint8_t level = active_brightness() / 4;
    set_brightness(level < 10 ? 10 : level, true);
    set_screen(POWER_SCREEN_DIM);
}

static void go_aod(void)
{
    set_brightness(aod_brightness(), true);
    set_screen(POWER_SCREEN_AOD);  /* the app manager shows the always-on face */
    vTaskDelay(pdMS_TO_TICKS(20));
    lv_port_refresh_now();
    if (panel_used()) touch_set_low_power(true);

    ESP_LOGI(TAG, "Always-on display (%u/255, touch wake %s)", aod_brightness(), settings_get()->tap_to_wake ? "on" : "off");
}

static void go_off(void)
{
    if (s_screen == POWER_SCREEN_OFF)
    {
        return;
    }

    set_brightness(0, s_screen == POWER_SCREEN_ON || s_screen == POWER_SCREEN_DIM);
    set_screen(POWER_SCREEN_OFF);
    lv_port_set_running(false);
    if (panel_used()) display_sleep(true);
    if (panel_used()) touch_set_low_power(true);

    ESP_LOGI(TAG, "Screen off");
}

static void go_idle(void)
{
    if (settings_get()->always_on)
    {
        go_aod();
    }
    else
    {
        go_off();
    }
}

/*
 * Remote mirror on: the UI stays "on" (rendering, no timeout) for the remote
 * client while the panel is dark and asleep. Off: the panel shows the current
 * screen again and the normal timeout restarts.
 */
static void set_mirror(bool on)
{
    if (on == s_mirror)
    {
        return;
    }

    if (on)
    {
        bool lit = s_screen != POWER_SCREEN_OFF;

        s_mirror = true;   /* first: go_on() must not light the panel */
        go_on();           /* logical screen on: apps show their normal screen */

        if (!lv_port_headless())
        {
            if (lit)
            {
                display_set_brightness(0);
                display_sleep(true);
            }

            touch_set_low_power(false);   /* a touch on the device takes it back */
        }

        ESP_LOGI(TAG, "Panel off: screen mirrored remotely");
        return;
    }

    s_mirror = false;
    atomic_store(&s_last_activity_us, now_us());

    if (s_screen == POWER_SCREEN_OFF)
    {
        return;   /* went to sleep meanwhile: the panel is already asleep */
    }

    if (!lv_port_headless())
    {
        display_sleep(false);
        touch_set_low_power(s_screen == POWER_SCREEN_AOD);
    }

    lv_port_refresh_now();
    s_brightness_now = 0;
    set_brightness(s_screen == POWER_SCREEN_AOD ? aod_brightness() : active_brightness(), true);
    ESP_LOGI(TAG, "Panel back on");
}

/* lvgl task: a button pressed on the device ends the remote mirror. */
static void stop_mirror_ui(void *arg)
{
    (void)arg;
    lv_port_mirror_stop(true);
}

/* ------------------------------------------------------------ inputs */

static void button_cb(button_id_t button, button_event_t event)
{
    if (button == BUTTON_BOOT && event == BUTTON_RECOVERY)
    {
        recovery_request(!recovery_active());
        return;
    }
    post(MSG_BUTTON, button, event);
}

/* lvgl task: arg = (button << 8) | event. */
static void run_button_handler(void *arg)
{
    intptr_t packed = (intptr_t)arg;

    if (s_button_handler != NULL)
    {
        s_button_handler((button_id_t)(packed >> 8), (button_event_t)(packed & 0xFF));
    }
}

static void handle_button(button_id_t button, button_event_t event)
{
    bool screen_was_on = s_screen == POWER_SCREEN_ON || s_screen == POWER_SCREEN_DIM;

    atomic_store(&s_last_activity_us, now_us());

    if (s_mirror)
    {
        ui_async(stop_mirror_ui, NULL);   /* the device is used locally again: nothing more */
        return;
    }

    if (!screen_was_on)
    {
        go_on();   /* any button wakes the screen, nothing more */
        return;
    }

    if (s_screen == POWER_SCREEN_DIM)
    {
        go_on();
    }

    if (s_button_handler != NULL)
    {
        /* Back / home / sleep are UI decisions: run them in the lvgl task. */
        ui_async(run_button_handler, (void *)(intptr_t)((button << 8) | event));
    }
}

static void poll_battery(void)
{
    if (!pmu_present())
    {
        return;
    }

    int percent = pmu_battery_percent();
    bool usb = pmu_usb_present();
    bool was_usb = state_get(STATE_USB) != 0;

    state_set(STATE_BATTERY, percent);
    state_set(STATE_CHARGING, pmu_is_charging());
    state_set(STATE_USB, usb);

    if (usb && !was_usb && s_screen != POWER_SCREEN_ON)
    {
        go_on();   /* show the charging state when the cable is plugged in */
    }
}

/*
 * Cable plugged in / out, checked with every PWR key poll (one PMU register,
 * the chip is awake for the key anyway). With power on the cable the chip
 * stays out of light sleep, so the USB port always answers the PC (flashing,
 * monitor); if the PC gave up on it while we slept, it is re-announced.
 */
static void check_usb_power(void)
{
    if (!pmu_present())
    {
        return;
    }

    bool usb = pmu_usb_present();

    if (usb == s_usb_power)
    {
        return;
    }

    s_usb_power = usb;

    if (usb)
    {
        esp_pm_lock_acquire(s_usb_lock);
        poll_battery();
        usb_console_reconnect();
    }
    else
    {
        esp_pm_lock_release(s_usb_lock);
        poll_battery();
    }

    ESP_LOGI(TAG, "USB power %s", usb ? "on: light sleep paused" : "off");
}

/* -------------------------------------------------------------- task */

/*
 * A stuck UI must not look like a dead watch until the battery runs out:
 * restart with a core dump (the next boot says where it was stuck). The power
 * task itself is watched by the task watchdog (it wakes at least every 300 ms).
 */
static void check_lvgl_stall(void)
{
    uint32_t stall_ms = lv_port_stall_ms();

    if (stall_ms >= LVGL_STALL_MS)
    {
        ESP_LOGE(TAG, "lvgl task stuck for %u ms: restarting", (unsigned)stall_ms);
        esp_system_abort("lvgl task stuck");
    }
}

static void power_task(void *arg)
{
    (void)arg;

    int64_t next_key_poll = 0;
    int64_t next_battery_poll = 0;

    esp_task_wdt_add(NULL);

    while (true)
    {
        esp_task_wdt_reset();
        check_lvgl_stall();

        int64_t now = now_us();

        /* Screen timeouts. */
        if ((s_screen == POWER_SCREEN_ON || s_screen == POWER_SCREEN_DIM) && atomic_load(&s_keep_on) == 0 && !s_mirror)
        {
            int64_t idle_ms = (now - atomic_load(&s_last_activity_us)) / 1000;
            int64_t timeout_ms = (int64_t)settings_get()->screen_timeout_s * 1000;
            if (settings_get()->energy_saver && timeout_ms > 10000) timeout_ms = 10000;

            if (idle_ms >= timeout_ms)
            {
                go_idle();
            }
            else if (idle_ms >= timeout_ms - DIM_BEFORE_OFF_MS)
            {
                go_dim();
            }
            else if (s_screen == POWER_SCREEN_DIM)
            {
                go_on();   /* touched while dimmed */
            }
        }
        else if (s_screen == POWER_SCREEN_DIM)
        {
            go_on();   /* someone asked to keep the screen on */
        }

        /* PWR key (latched by the PMU) and battery. */
        if (now >= next_key_poll)
        {
            button_poll_pmu();
            check_usb_power();
            next_key_poll = now + (int64_t)(s_screen == POWER_SCREEN_ON ? KEY_POLL_ON_MS : KEY_POLL_OFF_MS) * 1000;
        }

        if (now >= next_battery_poll)
        {
            poll_battery();
            next_battery_poll = now + (int64_t)(s_screen == POWER_SCREEN_OFF ? BATTERY_POLL_OFF_MS : BATTERY_POLL_ON_MS) * 1000;
        }

        /* Sleep until the next poll (or a message). */
        int64_t wake_at = next_key_poll < next_battery_poll ? next_key_poll : next_battery_poll;
        int64_t wait_us = wake_at - now_us();

        if (s_screen == POWER_SCREEN_ON || s_screen == POWER_SCREEN_DIM)
        {
            wait_us = wait_us > 100000 ? 100000 : wait_us;   /* check the timeout 10x/s */
        }

        TickType_t ticks = wait_us > 0 ? pdMS_TO_TICKS(wait_us / 1000) : 0;
        msg_t msg;

        if (xQueueReceive(s_queue, &msg, ticks) != pdTRUE)
        {
            continue;
        }

        switch (msg.type)
        {
            case MSG_BUTTON:
                handle_button((button_id_t)msg.a, (button_event_t)msg.b);
                break;

            case MSG_TOUCH_WAKE:
                if ((s_screen == POWER_SCREEN_AOD || s_screen == POWER_SCREEN_OFF) && settings_get()->tap_to_wake)
                {
                    go_on();
                }
                break;

            case MSG_WAKE:
                go_on();
                break;

            case MSG_SLEEP:
                go_idle();
                break;

            case MSG_MIRROR:
                set_mirror(msg.a != 0);
                break;

            case MSG_SETTINGS:
                if (s_screen == POWER_SCREEN_ON && !s_mirror)
                {
                    set_brightness(active_brightness(), false);
                }
                else if (s_screen == POWER_SCREEN_AOD && !s_mirror) set_brightness(aod_brightness(), false);
                break;
        }
    }
}

/* ------------------------------------------------------------ public */

esp_err_t power_inputs_init(void)
{
    if (s_inputs_started) return ESP_OK;
    esp_err_t err = button_init(button_cb);
    s_inputs_started = err == ESP_OK;
    return err;
}

esp_err_t power_init(void)
{
    /* Dynamic frequency + automatic light sleep whenever every task is idle. */
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&pm);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "PM config: %s", esp_err_to_name(err));
    }

    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "boost", &s_cpu_lock);
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_power", &s_usb_lock);

    s_queue = xQueueCreate(8, sizeof(msg_t));

    if (s_queue == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&s_last_activity_us, now_us());
    s_screen = POWER_SCREEN_OFF;   /* panel starts dark: go_on() lights it */
    s_brightness_now = 0;

    if (!sys_task_create(power_task, "power", TASK_STACK, NULL, TASK_PRIORITY, 0))
    {
        return ESP_ERR_NO_MEM;
    }

    err = power_inputs_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "Buttons: %s", esp_err_to_name(err));
    post(MSG_WAKE, POWER_WAKE_OTHER, 0);

    return ESP_OK;
}

void power_user_activity(void)
{
    atomic_store(&s_last_activity_us, now_us());
}

void power_touch_wake(void)
{
    post(MSG_TOUCH_WAKE, 0, 0);
}

void power_wake(power_wake_t reason)
{
    atomic_store(&s_last_activity_us, now_us());
    post(MSG_WAKE, reason, 0);
}

void power_sleep_now(void)
{
    post(MSG_SLEEP, 0, 0);
}

void power_keep_screen_on(bool keep)
{
    if (keep)
    {
        atomic_fetch_add(&s_keep_on, 1);
        power_wake(POWER_WAKE_OTHER);
    }
    else if (atomic_fetch_sub(&s_keep_on, 1) <= 1)
    {
        atomic_store(&s_keep_on, 0);
        atomic_store(&s_last_activity_us, now_us());
    }
}

power_screen_t power_screen(void)
{
    return s_screen;
}

void power_remote_mirror(bool on)
{
    post(MSG_MIRROR, on, 0);
}

void power_settings_changed(void)
{
    post(MSG_SETTINGS, 0, 0);
}

void power_set_button_handler(void (*handler)(button_id_t button, button_event_t event))
{
    s_button_handler = handler;
}

static void prepare_power_down(void)
{
    settings_flush();
    clock_checkpoint();
    display_set_brightness(0);
    if (!lv_port_headless()) display_sleep(true);
}

void power_restart(void)
{
    prepare_power_down();
    esp_restart();
}

void power_shutdown(void)
{
    prepare_power_down();
    pmu_power_off();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();   /* no PMU (or USB keeps us alive): at least restart */
}

void power_cpu_boost(bool on)
{
    if (s_cpu_lock == NULL)
    {
        return;
    }

    if (on)
    {
        if (atomic_fetch_add(&s_cpu_boost, 1) == 0)
        {
            esp_pm_lock_acquire(s_cpu_lock);
        }
    }
    else if (atomic_fetch_sub(&s_cpu_boost, 1) == 1)
    {
        esp_pm_lock_release(s_cpu_lock);
    }
}
