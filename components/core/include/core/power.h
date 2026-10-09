#ifndef CORE_POWER_H
#define CORE_POWER_H

/*
 * Power manager: the screen states and everything that saves battery.
 *
 *            touch / button                     timeout - 4 s
 *     ┌──────────────────────────┐        ┌──────────────────────┐
 *     ▼                          │        │                      ▼
 *   [ ON ] ── user idle ──► [ DIM ] ── timeout ──► [ AOD ]  or  [ OFF ]
 *     ▲                                              │            │
 *     └──── tap / BOOT / PWR / alarm / notification ─┴────────────┘
 *
 *   ON   full brightness, touch at full speed, CPU up to 240 MHz
 *   DIM  1/4 brightness: a hint that the screen is about to turn off
 *   AOD  "always on": dim watch face, redrawn once a minute
 *   OFF  panel asleep, touch in low-power scan, LVGL stopped
 *
 * Whenever nothing runs, the CPU drops to 40 MHz and the chip goes into
 * automatic light sleep (woken by touch INT, BOOT, timers, Wi-Fi/BLE).
 * Hardware is switched off when unused: audio, Wi-Fi (services), panel.
 */

#include <stdbool.h>

#include "esp_err.h"
#include "hardware/button.h"

typedef enum
{
    POWER_SCREEN_ON,
    POWER_SCREEN_DIM,
    POWER_SCREEN_AOD,
    POWER_SCREEN_OFF,
} power_screen_t;

typedef enum
{
    POWER_WAKE_BUTTON,
    POWER_WAKE_TOUCH,
    POWER_WAKE_ALARM,
    POWER_WAKE_NOTIFICATION,
    POWER_WAKE_CHARGER,
    POWER_WAKE_OTHER,
} power_wake_t;

/* Start the power task, buttons and battery monitoring. After lv_port_init(). */
esp_err_t power_init(void);

/* The user did something (cheap, any task, called on every touch read). */
void power_user_activity(void);

/* The touch chip fired while the screen was off (lvgl task). */
void power_touch_wake(void);

/* Turn the screen on (alarms, notifications...). Any task. */
void power_wake(power_wake_t reason);

/* Turn the screen off now. Any task. */
void power_sleep_now(void);

/* Keep the screen on while > 0 (flashlight, ringing alarm...). Any task. */
void power_keep_screen_on(bool keep);

power_screen_t power_screen(void);

/* Settings changed (brightness, timeout...): apply now. */
void power_settings_changed(void);

/*
 * What the buttons do while the screen is on is decided by the app manager
 * (back / home / sleep). It runs in the lvgl task.
 */
void power_set_button_handler(void (*handler)(button_id_t button, button_event_t event));

/* Save state, switch the panel off and restart / power off. */
void power_restart(void);
void power_shutdown(void);

/* Hold the CPU at full speed (rendering, decoding). Reference counted. */
void power_cpu_boost(bool on);

#endif
