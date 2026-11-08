#include "standalone_stubs/standalone_mock.h"
#include "../ecos_bus_standalone.c"
#include <stdio.h>

static void fire_retry(void)
{
    assert(timer_cb != NULL);
    timer_cb(NULL);
    assert(posted_fn != NULL);
    sys_fn_t fn = posted_fn;
    posted_fn = NULL;
    fn(NULL);
}

static void test_startup_faults(void)
{
    esp_err_t *faults[] = {&fail_event, &fail_init, &fail_storage, &fail_mode, &fail_start, &fail_ps};
    const int codes[] = {-10, -11, -12, -13, -14, -15};
    for (unsigned i = 0; i < sizeof(faults)/sizeof(faults[0]); ++i)
    {
        reset_mocks();
        *faults[i] = codes[i];
        assert(wifi_radio_acquire() == codes[i]);
        assert(s_references == 0);
        if (i < 2)
        {
            assert(!s_owned && !s_initialized && !s_started);
        }
        else
        {
            assert(!s_owned && !s_initialized && !s_started);
        }
    }
}

static void test_shared_lease_and_channel(void)
{
    reset_mocks();
    assert(wifi_radio_acquire() == ESP_OK);
    assert(wifi_radio_acquire() == ESP_OK);
    assert(s_references == 2 && call_init == 1 && call_start == 1);
    assert(wifi_radio_set_channel(6));
    assert(last_channel == 6 && wifi_radio_channel() == 6);
    assert(!wifi_radio_set_channel(0) && !wifi_radio_set_channel(14));
    fail_channel = -30;
    assert(!wifi_radio_set_channel(11));
    fail_channel = 0;
    wifi_radio_release();
    assert(s_references == 1 && s_owned && call_stop == 0);
    wifi_radio_release();
    assert(s_references == 0 && !s_owned && call_stop == 1 && call_deinit == 1);
    int previous_reads = call_get_channel;
    assert(wifi_radio_channel() == 0 && call_get_channel == previous_reads);
}

static void test_existing_event_loop(void)
{
    reset_mocks();
    fail_event = ESP_ERR_INVALID_STATE;
    assert(wifi_radio_acquire() == ESP_OK);
    assert(s_references == 1 && s_owned && call_init == 1);
    wifi_radio_release();
    assert(!s_owned);
}

static void test_channel_owners(void)
{
    reset_mocks();
    assert(wifi_radio_acquire() == ESP_OK);
    assert(wifi_radio_channel_reserve("SL02", 6));
    assert(wifi_radio_pinned_channel() == 6);
    assert(!wifi_radio_set_channel(11));
    assert(wifi_radio_channel() == 6);
    assert(!wifi_radio_channel_reserve("RHUB", 11));
    assert(wifi_radio_channel_reserve("RHUB", 6));
    wifi_radio_channel_release("SL02");
    assert(!wifi_radio_set_channel(11));
    wifi_radio_channel_release("RHUB");
    assert(wifi_radio_pinned_channel() == 0);
    fail_channel = -30;
    assert(!wifi_radio_channel_reserve("SL02", 11));
    assert(wifi_radio_pinned_channel() == 0);
    fail_channel = 0;
    assert(wifi_radio_set_channel(11));
    wifi_radio_release();
}

static void test_retry_stop_and_deinit(void)
{
    reset_mocks();
    assert(wifi_radio_acquire() == ESP_OK);
    fail_stop = -40;
    wifi_radio_release();
    assert(s_references == 0 && s_owned && s_started && timer_starts == 1);
    fail_stop = 0;
    fire_retry();
    assert(!s_owned && !s_started && !s_initialized);

    reset_mocks();
    assert(wifi_radio_acquire() == ESP_OK);
    fail_deinit = -41;
    wifi_radio_release();
    assert(s_owned && !s_started && s_initialized && timer_starts == 1);
    fail_deinit = 0;
    fire_retry();
    assert(!s_owned && !s_initialized);
}

static void test_retry_partial_start_and_post_failure(void)
{
    reset_mocks();
    fail_ps = -50;
    fail_stop = -51;
    assert(wifi_radio_acquire() == -50);
    assert(s_references == 0 && s_owned && s_started && timer_starts == 1);
    fail_stop = 0;
    fire_retry();
    assert(!s_owned && !s_started && !s_initialized);

    reset_mocks();
    assert(wifi_radio_acquire() == ESP_OK);
    fail_stop = -52;
    wifi_radio_release();
    fail_post = -53;
    timer_cb(NULL);
    assert(posted_fn == NULL && timer_starts == 2);
    fail_post = 0;
    fail_stop = 0;
    fire_retry();
    assert(!s_owned);
}

int main(void)
{
    test_startup_faults();
    test_existing_event_loop();
    test_channel_owners();
    test_shared_lease_and_channel();
    test_retry_stop_and_deinit();
    test_retry_partial_start_and_post_failure();
    puts("PASS: standalone radio lease, startup rollback, channel and deferred teardown retries");
    return 0;
}
