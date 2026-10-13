/*
 * Ombra sounds: a Geiger counter while finding a device, three beeps on alert.
 *
 * Geiger: random clicks (Poisson process) whose rate grows with the signal,
 * from about one per second far away to a crackle when the device is in hand.
 * Random timing is what makes it read as "radiation" rather than a metronome,
 * and the ear follows a rate change long before the eye reads a number.
 * The speaker task blocks in audio_write(), which paces the 20 ms blocks.
 */
#include "ombra.h"

#include "core/settings.h"
#include "core/sys.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hardware/audio.h"
#include "hardware/board.h"

#include <stdatomic.h>
#include <string.h>

#define RATE 16000
#define BLOCK 320 /* 20 ms */
#define CLICK 40  /* 2.5 ms */

static atomic_uint s_find;
static atomic_bool s_enabled = true, s_alert;
static TaskHandle_t s_task;
static uint32_t s_seed = 0x2468ACE1u;

static uint32_t next_random(void)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return s_seed;
}

static void open_speaker(bool *running)
{
    if (*running)
        return;
    *running = audio_start(RATE, 1) == ESP_OK;
    if (*running)
    {
        int volume = settings_get()->volume;
        audio_set_volume(volume < 35 ? 35 : volume);
    }
}

static void beeps(void)
{
    static int16_t block[BLOCK];
    for (int beep = 0; beep < 3; beep++)
    {
        /* 150 ms of 1.6 kHz square wave with soft edges, 100 ms silence. */
        for (int part = 0; part < 13; part++)
        {
            for (int i = 0; i < BLOCK; i++)
            {
                int n = part * BLOCK + i;
                int16_t level = part < 7 ? ((n / 5) & 1 ? 9000 : -9000) : 0;
                if (part < 7 && (n < 80 || n > 7 * BLOCK - 80))
                    level /= 3;
                block[i] = level;
            }
            audio_write(block, BLOCK);
        }
    }
}

static void sound_task(void *arg)
{
    (void)arg;
    static int16_t block[BLOCK];
    bool running = false;
    int click_left = 0;
    int32_t amplitude = 0;
    for (;;)
    {
        if (atomic_exchange(&s_alert, false) && atomic_load(&s_enabled))
        {
            open_speaker(&running);
            if (running)
                beeps();
        }
        uint32_t id = atomic_load(&s_find);
        ombra_device_t device;
        if (id == 0 || !atomic_load(&s_enabled) || !ombra_get(id, &device))
        {
            if (running)
            {
                audio_stop();
                running = false;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
            continue;
        }
        open_speaker(&running);
        if (!running)
        {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        /* Proximity 0..1 from -95 dBm to -40 dBm; a lost device stays quiet. */
        bool fresh = (uint32_t)(esp_timer_get_time() / 1000) - device.last_ms < 4000;
        int p1000 = fresh ? (device.rssi + 95) * 1000 / 55 : 0;
        p1000 = p1000 < 0 ? 0 : p1000 > 1000 ? 1000 : p1000;
        /* 0.6 .. 30 clicks per second, quadratic so the last metres speed up. */
        uint32_t rate_milli = fresh ? 600 + (uint32_t)p1000 * (uint32_t)p1000 * 29400u / 1000000u : 0;
        /* Probability per sample = rate / RATE, compared against 32-bit random numbers. */
        uint32_t threshold = (uint32_t)((uint64_t)rate_milli * 4294967u / RATE);
        for (int i = 0; i < BLOCK; i++)
        {
            if (click_left == 0 && next_random() < threshold)
            {
                click_left = CLICK;
                amplitude = 14000 + (int32_t)(next_random() % 6000);
            }
            if (click_left > 0)
            {
                /* Decaying noise burst: the dry tick of a Geiger tube. */
                int32_t noise = (int32_t)(next_random() & 0xFFFF) - 32768;
                block[i] = (int16_t)(noise * amplitude / 32768 * click_left / CLICK);
                click_left--;
            }
            else
                block[i] = 0;
        }
        audio_write(block, BLOCK);
    }
}

void ombra_sound_init(void)
{
    if (!board_info()->has_audio)
        return;
    s_seed ^= esp_random();
    sys_task_create(sound_task, "ombra_snd", 3072, NULL, 4, tskNO_AFFINITY);
    s_task = xTaskGetHandle("ombra_snd");
}

bool ombra_sound_available(void) { return board_info()->has_audio; }

void ombra_sound_find(uint32_t id)
{
    atomic_store(&s_find, id);
    if (s_task)
        xTaskNotifyGive(s_task);
}

void ombra_sound_enable(bool on)
{
    atomic_store(&s_enabled, on);
    if (s_task)
        xTaskNotifyGive(s_task);
}

bool ombra_sound_enabled(void) { return atomic_load(&s_enabled); }

void ombra_sound_alert(void)
{
    atomic_store(&s_alert, true);
    if (s_task)
        xTaskNotifyGive(s_task);
}
