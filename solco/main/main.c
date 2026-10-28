/* Standalone DJ and Studio; the optional paired hub starts its radio on request. */
#include "core/app.h"
#include "core/clock.h"
#include "core/devcon.h"
#include "core/lv_port.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sys.h"
#include "engine.h"
#if SOLCO_LINK
#include "link.h"
#endif
#include "vinyl.h"
#include "esp_log.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "hardware/imu.h"
#include "install.h"
#include "solco_ui.h"
#include "ui/ui.h"
#include <stdio.h>
#include <string.h>

/* Development commands never change the user's master volume. */
bool devcon_app_command(char *line)
{
#if CONFIG_WATCH_DEV_CONSOLE
#if SOLCO_LINK
    if (solco_link_console(line))
        return true;
#endif
    int type, value, velocity;
    if(!strcmp(line,"solco-layout")) {
        solco_ui_info_t ui;
        lv_port_lock();
        solco_ui_info(&ui);
        lv_port_unlock();
        printf("@LAYOUT width=%d height=%d landscape=%d studio=%d deck=%d detail=%d library=%d view=%d "
               "master=%d\n",
               ui.width, ui.height, ui.landscape, ui.studio, ui.deck, ui.view == SOLCO_VIEW_FOCUS,
               ui.view == SOLCO_VIEW_LIBRARY, ui.view, ui.master);
        printf("@OK solco-layout\n");
        return true;
    }
    if (sscanf(line, "studio-motion %d", &value) == 1)
    {
        solco_motion_mode(value);
        printf("@OK studio-motion\n");
        return true;
    }
    if (sscanf(line, "studio-event %d %d %d", &type, &value, &velocity) == 3)
    {
        solco_studio_command(type, value, velocity);
        printf("@OK studio-event\n");
        return true;
    }
    if (sscanf(line, "studio-view %d", &value) == 1 && value >= -1 && value <= 3)
    {
        lv_port_lock();
        solco_studio_open(value);
        lv_port_unlock();
        printf("@OK studio-view\n");
        return true;
    }
    if (!strcmp(line, "studio-saved"))
    {
        printf("@SAVED match=%d\n", solco_studio_saved());
        printf("@OK studio-saved\n");
        return true;
    }
    if (!strcmp(line, "studio-burst"))
    {
        for (int bank = 0; bank < 32; bank++)
        {
            solco_studio_command(STUDIO_HIT, bank, 127);
        }
        printf("@OK studio-burst\n");
        return true;
    }
    int deck, action;
    if (sscanf(line, "deck-keylock %d %d", &deck, &action) == 2)
    {
        printf("@KEYLOCK accepted=%d\n", solco_keylock(deck, action != 0));
        printf("@OK deck-keylock\n");
        return true;
    }
    if (sscanf(line, "deck-rate %d %d", &deck, &action) == 2)
    {
        printf("@RATE accepted=%d\n", solco_rate(deck, action));
        printf("@OK deck-rate\n");
        return true;
    }
    if (sscanf(line, "deck-pitch %d %d", &deck, &action) == 2)
    {
        /* Fine pitch in ppm around forward 1x, e.g. 80000 = +8 %. */
        int64_t rate = SOLCO_SAMPLE_ONE + SOLCO_SAMPLE_ONE / 1000000 * action +
                       SOLCO_SAMPLE_ONE % 1000000 * action / 1000000;
        printf("@PITCH accepted=%d\n", solco_rate_fine(deck, rate));
        printf("@OK deck-pitch\n");
        return true;
    }
    if (!strcmp(line, "solco-perf"))
    {
        uint32_t histogram[8], restarts, dropped;
        solco_mix_histogram(histogram, &restarts, &dropped);
        printf("@MIXHIST %lu %lu %lu %lu %lu %lu %lu %lu restarts=%lu dropped=%lu\n",
               (unsigned long)histogram[0], (unsigned long)histogram[1],
               (unsigned long)histogram[2], (unsigned long)histogram[3],
               (unsigned long)histogram[4], (unsigned long)histogram[5],
               (unsigned long)histogram[6], (unsigned long)histogram[7], (unsigned long)restarts,
               (unsigned long)dropped);
        printf("@OK solco-perf\n");
        return true;
    }
    if (sscanf(line, "solco-profile %d", &value) == 1)
    {
        printf("@PROFILE accepted=%d name=%s\n", solco_audio_profile(value),
               solco_audio_profile_name(value));
        printf("@OK solco-profile\n");
        return true;
    }
    if (!strcmp(line, "solco-audio-restart"))
    {
        solco_test_audio_restart();
        printf("@OK solco-audio-restart\n");
        return true;
    }
    if (sscanf(line, "deck-loop %d %d", &deck, &action) == 2)
    {
        printf("@LOOP accepted=%d\n", solco_loop(deck, action));
        printf("@OK deck-loop\n");
        return true;
    }
    if (sscanf(line, "deck-play %d", &deck) == 1 && deck >= 0 && deck < 2)
    {
        printf("@PLAY accepted=%d\n", solco_toggle(deck));
        printf("@OK deck-play\n");
        return true;
    }
    if (sscanf(line, "solco-view %d %d", &action, &deck) == 2 && deck >= 0 && deck < 2)
    {
        lv_port_lock();
        solco_ui_show(action, deck);
        lv_port_unlock();
        printf("@OK solco-view\n");
        return true;
    }
    if (sscanf(line, "deck-open %d", &deck) == 1 && deck >= 0 && deck < 2)
    {
        lv_port_lock();
        solco_ui_show(SOLCO_VIEW_FOCUS, deck);
        lv_port_unlock();
        printf("@OK deck-open\n");
        return true;
    }
    if (!strcmp(line, "studio"))
    {
        studio_status_t state;
        solco_studio_status(&state);
        printf("@STUDIO init_us=%lu kit=%d bpm=%d play=%d rec=%d step=%d voices=%d hits=%lu "
               "drop=%lu late=%lu "
               "frame=%llu "
               "revision=%lu pattern=%u/%u/%u/%u/%u/%u/%u/%u\n",
               (unsigned long)solco_studio_init_us(), state.kit, state.bpm, state.playing,
               state.recording, state.step, state.voices, (unsigned long)state.hits,
               (unsigned long)state.dropped, (unsigned long)state.late,
               (unsigned long long)state.frame, (unsigned long)state.revision,
               state.pattern.pattern[0], state.pattern.pattern[1], state.pattern.pattern[2],
               state.pattern.pattern[3], state.pattern.pattern[4], state.pattern.pattern[5],
               state.pattern.pattern[6], state.pattern.pattern[7]);
        printf("@LINK transport=off audio=local\n");
        solco_ui_info_t ui;
        lv_port_lock();
        solco_ui_info(&ui);
        lv_port_unlock();
        printf("@VIEW studio=%d\n", ui.studio);
        printf("@UI deck_menu=0 studio_tools=%d view=%d master=%d\n", ui.studio_tools, ui.view, ui.master);
        uint32_t reads, errors, hits;
        solco_motion_stats(&reads, &errors, &hits);
        int learned, selected_pad;
        solco_motion_state(&learned, &selected_pad);
        printf("@GESTURE learned=%d pad=%d\n", learned, selected_pad);
        printf("@MOTION supported=%d mode=%d reads=%lu errors=%lu hits=%lu\n", imu_supported(),
               solco_motion_get(), (unsigned long)reads, (unsigned long)errors,
               (unsigned long)hits);
        printf("@OK studio\n");
        return true;
    }
    if (!strcmp(line, "solco-load-stress"))
    {
        printf(solco_test_load_stress() ? "@OK solco-load-stress\n" : "@ERR queue busy\n");
        return true;
    }
    if (!strcmp(line, "solco-mp3-reset"))
    {
        printf(solco_test_mp3_reset() ? "@OK solco-mp3-reset\n" : "@ERR fixture repair failed\n");
        return true;
    }
    if (!strcmp(line, "solco-mp3-check"))
    {
        solco_test_mp3_verify();
        printf("@OK solco-mp3-check\n");
        return true;
    }
    if (!strcmp(line, "solco-mp3-fixture"))
    {
        printf(solco_test_mp3_fixture() ? "@OK solco-mp3-fixture\n"
                                        : "@ERR fixture exists or SD unavailable\n");
        return true;
    }
    if (!strcmp(line, "solco-load-mp3"))
    {
        printf(solco_test_load_mp3() ? "@OK solco-load-mp3\n" : "@ERR queue busy\n");
        return true;
    }
    if (!strcmp(line, "solco-fixture"))
    {
        printf(solco_test_fixture() ? "@OK solco-fixture\n"
                                    : "@ERR fixture not created (exists or SD unavailable)\n");
        return true;
    }
    if (!strcmp(line, "solco-load-test"))
    {
        printf(solco_test_load() ? "@OK solco-load-test\n" : "@ERR queue busy\n");
        return true;
    }
#endif
    if (strcmp(line, "solco"))
    {
        return false;
    }
    solco_status_t status;
    solco_status(&status);
    printf("@SOLCO volume=%d cross=%d playing=%d/%d peak=%u empty=%lu errors=%lu clips=%lu "
           "launcher=%d\n",
           status.volume, status.cross, status.playing[0], status.playing[1], status.peak,
           (unsigned long)status.underruns, (unsigned long)status.audio_errors,
           (unsigned long)status.clips, solco_launcher_state());
    printf("@VINYL pos=%lu/%lu speed=%ld/%ld held=%d/%d ready=%d/%d mix_us=%lu\n",
           (unsigned long)status.frames[0], (unsigned long)status.frames[1], (long)status.speed[0],
           (long)status.speed[1], status.scratching[0], status.scratching[1], status.ready[0],
           status.ready[1], (unsigned long)status.mix_us);
    printf("@PREP busy=%d/%d percent=%d/%d tracks=%d\n", status.preparing[0], status.preparing[1],
           status.prepare_percent[0], status.prepare_percent[1], solco_library_count());
    printf("@CACHE miss=%lu/%lu tag=%lu/%lu wheel=%d\n", (unsigned long)status.miss_frame[0],
           (unsigned long)status.miss_frame[1], (unsigned long)status.miss_tag[0],
           (unsigned long)status.miss_tag[1], solco_ui_wheel_vinyl());
    printf("@TIMING cycle_us=%lu late=%lu blocks=%lu profile=%s block=%d queue_frames=%d "
           "queue_us=%ld\n",
           (unsigned long)status.cycle_us, (unsigned long)status.late_cycles,
           (unsigned long)status.blocks, solco_audio_profile_name(status.profile), status.block,
           status.queue_frames, (long)((int64_t)status.queue_frames * 1000000 / 44100));
    if (status.error[0])
    {
        printf("@ERROR %s\n", status.error);
    }
    printf("@KEYLOCK enabled=%d/%d\n", status.keylock[0], status.keylock[1]);
    printf("@RATE value=%ld/%ld ppm=%ld/%ld\n", (long)status.rate[0], (long)status.rate[1],
           (long)status.rate_ppm[0], (long)status.rate_ppm[1]);
    printf("@LOOPS active=%d/%d in=%lu/%lu out=%lu/%lu\n", status.looping[0], status.looping[1],
           (unsigned long)status.loop_in[0], (unsigned long)status.loop_in[1],
           (unsigned long)status.loop_out[0], (unsigned long)status.loop_out[1]);
    printf("@OK solco\n");
    return true;
}

void app_main(void)
{
    ESP_ERROR_CHECK(extapp_sdk_init());
    sys_check_last_reset();
    sys_worker_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    /* Solco uses the long edge horizontally; launcher orientation is untouched. */
    bool rectangle=!board_info()->round && board_info()->width!=board_info()->height;
    ESP_ERROR_CHECK(lv_port_init_orientation(rectangle && board_info()->width<board_info()->height ? 1 : 0));
    clock_init();
    solco_engine_sync_init();
    lv_port_lock();
    ui_init();
    solco_ui_create();
    lv_port_unlock();
    app_manager_init();
    ESP_ERROR_CHECK(power_init());
    power_keep_screen_on(true);
    /* BOOT click = MASTER sheet, hold = back; leaving needs a hold in MASTER. */
    power_set_button_handler(solco_ui_button);
    ESP_ERROR_CHECK(solco_engine_start());
    devcon_start();
    sys_post(solco_install_launcher, NULL);
    sys_heap_log("solco ready");
    ESP_LOGI("solco", "Ready: %s; dual-deck PCM16 44100 Hz; %d ch; volume 0%%", board_info()->name,
             SOLCO_OUTPUT_CHANNELS);
}
