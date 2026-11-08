#include "timer_stubs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern int64_t fake_now_us;
extern esp_err_t create_result, stop_result, start_result;
extern int create_calls, stop_calls, start_calls, alarm_stop_calls, watch_label_calls;
extern uint64_t last_start_us;

static void reset(void)
{
    s_watch = (stopwatch_t){0};
    s_countdown = NULL;
    s_countdown_end_us = 0;
    s_countdown_minutes = 5;
    s_countdown_duration_us = 0;
    s_watch_button = NULL;
    fake_now_us = 1000000;
    create_result = stop_result = start_result = ESP_OK;
    create_calls = stop_calls = start_calls = alarm_stop_calls = watch_label_calls = 0;
    last_start_us = 0;
}

static void test_invalid_duration_is_side_effect_free(void)
{
    reset();
    assert(!timer_remote("start", 0));
    assert(!timer_remote("start", -1));
    assert(!timer_remote("start", 24 * 3600 + 1));
    assert(create_calls == 0 && stop_calls == 0 && start_calls == 0);
    assert(s_countdown_end_us == 0 && s_countdown_duration_us == 0);
}

static void test_create_and_stop_faults_leave_state_truthful(void)
{
    reset();
    create_result = ESP_FAIL;
    assert(!timer_remote("start", 60));
    assert(create_calls == 1 && start_calls == 0 && s_countdown == NULL);
    assert(s_countdown_end_us == 0 && s_countdown_duration_us == 0);

    reset();
    s_countdown = &timer_instance;
    s_countdown_end_us = 9000000;
    s_countdown_duration_us = 8000000;
    stop_result = ESP_FAIL;
    assert(!timer_remote("start", 60));
    assert(stop_calls == 1 && start_calls == 0 && alarm_stop_calls == 0);
    assert(s_countdown_end_us == 9000000 && s_countdown_duration_us == 8000000);
}

static void test_failed_start_clears_running_state(void)
{
    reset();
    start_result = ESP_FAIL;
    assert(!timer_remote("start", 60));
    assert(create_calls == 1 && stop_calls == 1 && start_calls == 1);
    assert(last_start_us == 60000000 && alarm_stop_calls == 1);
    assert(s_countdown_end_us == 0 && s_countdown_duration_us == 0);

    reset();
    s_countdown = &timer_instance;
    stop_result = ESP_ERR_INVALID_STATE; /* inactive timer is safe to start */
    assert(timer_remote("start", 90));
    assert(stop_calls == 1 && start_calls == 1);
    assert(s_countdown_end_us == fake_now_us + 90000000);
    assert(s_countdown_duration_us == 90000000 && s_countdown_minutes == 1);
}

static void test_stop_is_idempotent_and_reports_real_failure(void)
{
    reset();
    s_countdown = &timer_instance;
    s_countdown_end_us = 5000000;
    s_countdown_duration_us = 4000000;
    stop_result = ESP_FAIL;
    assert(!timer_remote("stop", 0));
    assert(s_countdown_end_us == 5000000 && s_countdown_duration_us == 4000000);
    assert(alarm_stop_calls == 0);

    stop_result = ESP_OK;
    assert(timer_remote("stop", 0));
    assert(s_countdown_end_us == 0 && s_countdown_duration_us == 0);
    assert(stop_calls == 2 && alarm_stop_calls == 1);

    stop_result = ESP_ERR_INVALID_STATE;
    assert(timer_remote("stop", 0));
    assert(s_countdown_end_us == 0 && s_countdown_duration_us == 0);
}

static void test_watch_start_pause_reset(void)
{
    reset();
    assert(timer_remote("watch", 0));
    assert(s_watch.running && s_watch.started_us == fake_now_us);
    fake_now_us += 2500000;
    assert(timer_remote("watch", 0));
    assert(!s_watch.running && s_watch.accumulated_us == 2500000);
    assert(timer_remote("watch_reset", 0));
    assert(!s_watch.running && s_watch.accumulated_us == 0 && watch_label_calls == 0);
    assert(!timer_remote("unknown", 0));
}

int main(void)
{
    test_invalid_duration_is_side_effect_free();
    test_create_and_stop_faults_leave_state_truthful();
    test_failed_start_clears_running_state();
    test_stop_is_idempotent_and_reports_real_failure();
    test_watch_start_pause_reset();
    puts("timer remote tests passed");
    return 0;
}
