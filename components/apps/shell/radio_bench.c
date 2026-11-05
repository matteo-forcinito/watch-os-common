/*
 * Orbit phase 1 (docs/ORBIT-ESPNOW.md): internal RAM in each radio state.
 * QA console only: "radios" walks through
 *
 *   off, ble, espnow, espnow+ble, wifi (joined), wifi+ble, off again
 *
 * and prints one line per state:
 *
 *   @RADIO <state> free=<KB> largest=<KB> min=<KB> psram=<KB> ok=<0|1>
 *
 * free/largest: internal RAM now and its largest block (what a new task or the
 * Bluetooth controller needs); min: the lowest free internal RAM since boot.
 * Each radio is released before the next state; the run takes about a minute.
 */
#include "apps_internal.h"

#include "companion/ble_companion.h"
#include "core/sys.h"
#include "services/wifi.h"

#include "esp_heap_caps.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#define SETTLE_MS 3000   /* allocations of a radio coming up or going down */

static void measure(const char *state, bool ok)
{
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    wifi_activity_t radio = {0};
    wifi_activity_snapshot(&radio);
    printf("@RADIO %-11s free=%u largest=%u min=%u psram=%u ok=%d ble=%d wifi=%d\n", state,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), ok ? 1 : 0,
           ble_companion_running() ? 1 : 0, radio.started ? 1 : 0);
    fflush(stdout);
}

static bool espnow_on(void)
{
    if (wifi_radio_acquire() != ESP_OK)
    {
        return false;
    }
    if (esp_now_init() != ESP_OK)
    {
        wifi_radio_release();
        return false;
    }
    return true;
}

static void espnow_off(void)
{
    esp_now_deinit();
    wifi_radio_release();
}

static void ble(bool on)
{
    ble_companion_hold(on);
    /* The controller starts and stops in the worker: wait for it, at most 10 s. */
    for (int i = 0; i < 100 && ble_companion_running() != on; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void wait_radio_off(void)
{
    /* The Wi-Fi service switches the radio off after its grace period. */
    wifi_activity_t radio;
    for (int i = 0; i < 300; i++)
    {
        if (wifi_activity_snapshot(&radio) && !radio.started && !radio.driver)
        {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool radio_bench_command(const char *line)
{
    if (strcmp(line, "radios") != 0)
    {
        return false;
    }
    printf("@RADIO start (about a minute)\n");
    measure("off", true);

    ble(true);
    measure("ble", true);
    ble(false);
    measure("ble-off", true);

    bool ok = espnow_on();
    measure("espnow", ok);
    ble(true);
    measure("espnow+ble", ok);
    ble(false);
    if (ok) espnow_off();
    wait_radio_off();
    measure("espnow-off", true);

    ok = wifi_acquire(15000) == ESP_OK;
    measure("wifi", ok);
    ble(true);
    measure("wifi+ble", ok);
    ble(false);
    if (ok) wifi_release_now();
    wait_radio_off();

    measure("off-end", true);
    printf("@OK radios\n");
    return true;
}
