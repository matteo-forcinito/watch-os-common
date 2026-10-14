/*
 * Finder sounds: a short tick whose rate follows the heat (cold: about one a
 * second, hot: a fast patter), and a two-tone "here" when the repeater spot is
 * reached. Like a metal detector, the ear follows the change long before the
 * eye reads a number. A ringing alarm owns the speaker: then we stay silent.
 */
#include "esp_attr.h"
#include "copertura.h"

#include "core/settings.h"
#include "core/sys.h"
#include "hardware/audio.h"
#include "hardware/board.h"
#include "services/sound.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>

#define RATE  16000
#define BLOCK 320   /* 20 ms */

static atomic_int s_heat = -1;
static atomic_bool s_target;
static atomic_bool s_quit;
static atomic_bool s_running;

void cp_sound_heat(int heat, bool on_target)
{
    atomic_store(&s_heat, heat);
    atomic_store(&s_target, on_target);
}

static void tone(int16_t *block, int frequency, int amplitude, int phase_start)
{
    for (int i = 0; i < BLOCK; i++)
    {
        int n = phase_start + i;
        /* Square wave, soft edges by the speaker itself. */
        block[i] = (int16_t)(((n * frequency * 2 / RATE) & 1) ? amplitude : -amplitude);
    }
}

static void sound_task(void *arg)
{
    (void)arg;
    EXT_RAM_BSS_ATTR static int16_t block[BLOCK];
    bool running = false;
    int since_tick = 1000;
    for (;;)
    {
        if (atomic_load(&s_quit))
        {
            if (running && !sound_alarm_playing())
            {
                audio_stop();
            }
            atomic_store(&s_running, false);
            vTaskDelete(NULL);
            return;
        }
        int heat = atomic_load(&s_heat);
        if (heat < 0 || sound_alarm_playing())
        {
            if (running && !sound_alarm_playing())
            {
                audio_stop();
            }
            running = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!running)
        {
            running = audio_start(RATE, 1) == ESP_OK;
            if (!running)
            {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            int volume = settings_get()->volume;
            audio_set_volume(volume < 40 ? 40 : volume);
        }
        /* Interval between ticks: 1000 ms cold .. 90 ms hot. */
        int interval = 1000 - heat * 910 / 100;
        if (atomic_load(&s_target))
        {
            /* On target: a steady two-tone every 600 ms. */
            if (since_tick >= 600)
            {
                for (int b = 0; b < 4; b++)
                {
                    tone(block, b < 2 ? 1320 : 1760, 7000, b * BLOCK);
                    audio_write(block, BLOCK);
                }
                since_tick = 80;
            }
        }
        else if (since_tick >= interval)
        {
            tone(block, 900 + heat * 8, 9000, 0);
            for (int i = 80; i < BLOCK; i++)
            {
                block[i] = 0;   /* 5 ms tick, then silence */
            }
            audio_write(block, BLOCK);
            since_tick = 20;
            continue;
        }
        for (int i = 0; i < BLOCK; i++)
        {
            block[i] = 0;
        }
        audio_write(block, BLOCK);   /* paces the loop at 20 ms */
        since_tick += 20;
    }
}

void cp_sound_init(void)
{
    atomic_store(&s_quit, false);
    if (board_info()->has_audio && !atomic_exchange(&s_running, true))
    {
        if (!sys_task_create(sound_task, "cp_sound", 3072, NULL, 4, SYS_CORE_ANY))
        {
            atomic_store(&s_running, false);
        }
    }
}

void cp_sound_stop(void)
{
    atomic_store(&s_heat, -1);
    atomic_store(&s_quit, true);
}
