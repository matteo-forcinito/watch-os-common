#include <assert.h>
#include <stdio.h>

static void test_init_failure_destroys_owned_netifs(void)
{
    reset_wifi_stubs();
    init_result = ESP_FAIL;
    assert(!driver_up());
    assert(init_calls == 1 && deinit_calls == 0);
    assert(destroy_calls == 2 && s_sta == NULL && s_ap == NULL);
    assert(!s_driver && !s_driver_ready);
}

static void test_storage_failure_rolls_back_or_retries_cleanup(void)
{
    reset_wifi_stubs();
    storage_result = ESP_FAIL;
    assert(!driver_up());
    assert(init_calls == 1 && storage_calls == 1 && deinit_calls == 1);
    assert(destroy_calls == 2 && s_sta == NULL && s_ap == NULL);
    assert(!s_driver && !s_driver_ready);

    reset_wifi_stubs();
    storage_result = ESP_FAIL;
    deinit_result = ESP_FAIL;
    assert(!driver_up());
    assert(s_driver && !s_driver_ready && s_sta != NULL && s_ap != NULL);
    assert(destroy_calls == 0 && deinit_calls == 1);
    deinit_result = ESP_OK;
    storage_result = ESP_OK;
    assert(driver_up());
    assert(deinit_calls == 2 && destroy_calls == 2);
    assert(create_sta_calls == 2 && create_ap_calls == 2);
    assert(s_driver && s_driver_ready && s_sta != NULL && s_ap != NULL);
}

static void test_initial_mode_and_start_failures_release_driver(void)
{
    reset_wifi_stubs();
    mode_result = ESP_FAIL;
    assert(!radio_on(WIFI_MODE_STA));
    assert(mode_calls == 1 && start_calls == 0 && deinit_calls == 1);
    assert(destroy_calls == 2 && s_sta == NULL && s_ap == NULL);
    assert(!s_driver && !s_driver_ready && !s_started);

    reset_wifi_stubs();
    start_result = ESP_FAIL;
    assert(!radio_on(WIFI_MODE_STA));
    assert(mode_calls == 1 && start_calls == 1 && deinit_calls == 1);
    assert(destroy_calls == 2 && s_sta == NULL && s_ap == NULL);
    assert(!s_driver && !s_driver_ready && !s_started);
}

static void test_started_radio_survives_mode_failure(void)
{
    reset_wifi_stubs();
    s_driver = s_driver_ready = s_started = true;
    s_sta = &sta_netif;
    s_ap = &ap_netif;
    mode_result = ESP_FAIL;
    assert(!radio_on(WIFI_MODE_APSTA));
    assert(deinit_calls == 0 && destroy_calls == 0);
    assert(s_driver && s_driver_ready && s_started);
    assert(s_sta == &sta_netif && s_ap == &ap_netif);
}

static void test_radio_off_cleans_driver_even_if_already_stopped(void)
{
    reset_wifi_stubs();
    s_driver = s_driver_ready = true;
    s_sta = &sta_netif;
    s_ap = &ap_netif;
    s_sta_wanted = true;
    assert(!s_started);
    radio_off();
    assert(stop_calls == 0 && deinit_calls == 1 && destroy_calls == 2);
    assert(!s_driver && !s_driver_ready && s_sta == NULL && s_ap == NULL);
    assert(!s_sta_wanted && state_calls == 1 && last_state_value == STATE_WIFI_OFF);
}

static void test_off_job_preserves_owners_and_retries_cleanup(void)
{
    reset_wifi_stubs();
    s_refs = 1;
    s_driver = s_driver_ready = s_started = true;
    s_sta = &sta_netif;
    s_ap = &ap_netif;
    off_job(NULL);
    assert(mutex_take_calls == 1 && mutex_give_calls == 1);
    assert(stop_calls == 0 && deinit_calls == 0 && destroy_calls == 0 && timer_calls == 0);

    s_refs = 0;
    s_ap_on = true;
    stop_result = ESP_FAIL;
    off_job(NULL);
    assert(stop_calls == 1 && deinit_calls == 0 && destroy_calls == 0);
    assert(s_started && s_driver && s_ap_on && s_sta == &sta_netif && s_ap == &ap_netif);
    assert(timer_calls == 1);

    stop_result = ESP_OK;
    deinit_result = ESP_FAIL;
    off_job(NULL);
    assert(stop_calls == 2 && deinit_calls == 1 && destroy_calls == 0);
    assert(!s_started && s_driver && s_driver_ready && !s_ap_on);
    assert(s_sta == &sta_netif && s_ap == &ap_netif && timer_calls == 2);

    deinit_result = ESP_OK;
    off_job(NULL);
    assert(stop_calls == 2 && deinit_calls == 2 && destroy_calls == 2);
    assert(!s_started && !s_driver && !s_driver_ready && s_sta == NULL && s_ap == NULL);
    assert(timer_calls == 2 && last_state_value == STATE_WIFI_OFF);
}

static void test_off_timer_rearms_when_sys_queue_is_full(void)
{
    reset_wifi_stubs();
    post_result = false;
    off_timer_cb(NULL);
    assert(post_calls == 1 && timer_calls == 1);

    post_result = true;
    off_timer_cb(NULL);
    assert(post_calls == 2 && timer_calls == 1);
}

static void test_radio_only_release_disconnects_and_restores_pin(void)
{
    static const uint8_t owner[4] = {'H', 'I', 'D', '1'};
    reset_wifi_stubs();
    s_refs = 2;
    s_radio_refs = 2;
    s_driver = s_driver_ready = s_started = true;
    s_sta = &sta_netif;
    s_ap = &ap_netif;
    s_sta_wanted = true;
    event_bits = CONNECTED_BIT;
    sdk_channel = 11;
    assert(wifi_channel_reserve(&s_channel_policy, owner, 6, false, 11));

    off_job(NULL);
    assert(disconnect_calls == 1 && s_started && s_driver);
    assert(!s_sta_wanted && (event_bits & CONNECTED_BIT) == 0);
    assert(set_channel_calls == 1 && sdk_channel == 6);
    assert(stop_calls == 0 && deinit_calls == 0 && destroy_calls == 0);
    assert(timer_calls == 0);
}

static void test_radio_only_disconnect_failures_retry_safely(void)
{
    static const uint8_t owner[4] = {'H', 'I', 'D', '1'};
    reset_wifi_stubs();
    s_refs = 1;
    s_radio_refs = 1;
    s_started = true;
    s_sta = &sta_netif;
    s_ap = &ap_netif;
    s_sta_wanted = true;
    event_bits = CONNECTED_BIT;
    sdk_channel = 11;
    assert(wifi_channel_reserve(&s_channel_policy, owner, 6, false, 11));

    disconnect_result = ESP_FAIL;
    off_job(NULL);
    assert(disconnect_calls == 1 && (event_bits & CONNECTED_BIT) != 0);
    assert(set_channel_calls == 0 && sdk_channel == 11);
    assert(timer_calls == 1 && last_timer_delay == 1000000);
    assert(stop_calls == 0 && deinit_calls == 0);

    disconnect_result = ESP_OK;
    set_channel_result = ESP_FAIL;
    off_job(NULL);
    assert(disconnect_calls == 2 && (event_bits & CONNECTED_BIT) == 0);
    assert(set_channel_calls == 1 && sdk_channel == 11);
    assert(timer_calls == 2 && last_timer_delay == 1000000);
    assert(stop_calls == 0 && deinit_calls == 0);

    disconnect_result = ESP_ERR_WIFI_NOT_CONNECT;
    set_channel_result = ESP_OK;
    off_job(NULL);
    assert(disconnect_calls == 3 && set_channel_calls == 2 && sdk_channel == 6);
    assert(timer_calls == 2 && stop_calls == 0 && deinit_calls == 0);
}

static void test_radio_only_off_job_respects_new_ip_ap_and_scan_owners(void)
{
    reset_wifi_stubs();
    s_refs = 3;
    s_radio_refs = 2;
    s_started = true;
    event_bits = CONNECTED_BIT;
    off_job(NULL);
    assert(disconnect_calls == 0 && (event_bits & CONNECTED_BIT) != 0);

    s_refs = s_radio_refs;
    s_ap_on = true;
    off_job(NULL);
    assert(disconnect_calls == 0);
    s_ap_on = false;
    s_scans = 1;
    off_job(NULL);
    assert(disconnect_calls == 0);
}

static void test_release_counts_preserve_radio_holders(void)
{
    reset_wifi_stubs();
    s_refs = 2;
    s_radio_refs = 2;
    wifi_release();
    assert(s_refs == 2 && timer_calls == 0);
    wifi_release_now();
    assert(s_refs == 2 && timer_calls == 0);

    s_refs = 3;
    wifi_release();
    assert(s_refs == 2 && timer_calls == 1 && last_timer_delay == 8000000);
    s_refs = 3;
    wifi_release_now();
    assert(s_refs == 2 && timer_calls == 2 && last_timer_delay == 1000);

    s_refs = 4;
    s_radio_refs = 0;
    wifi_release();
    wifi_release();
    assert(s_refs == 2 && timer_calls == 2);
    wifi_release();
    assert(s_refs == 1 && timer_calls == 2);
    wifi_release();
    assert(s_refs == 0 && timer_calls == 3 && last_timer_delay == 8000000);

    s_refs = 2;
    s_radio_refs = 2;
    wifi_radio_release();
    assert(s_refs == 1 && s_radio_refs == 1 && timer_calls == 4 && last_timer_delay == 8000000);
}

static void test_channel_api_obeys_shared_policy_and_network_authority(void)
{
    static const char owner_a[4] = {'H', 'I', 'D', '1'};
    static const char owner_b[4] = {'S', 'O', 'L', 'C'};
    reset_wifi_stubs();
    assert(!wifi_radio_channel_reserve(owner_a, 6)); /* service not started */
    assert(get_channel_calls == 0 && set_channel_calls == 0);

    s_started = true;
    sdk_channel = 11;
    assert(wifi_radio_channel_reserve(owner_a, 6));
    assert(wifi_radio_pinned_channel() == 6 && sdk_channel == 6);
    assert(set_channel_calls == 1);
    assert(wifi_radio_channel_reserve(owner_b, 6));
    assert(!wifi_radio_channel_reserve(owner_b, 11));
    assert(wifi_radio_pinned_channel() == 6 && sdk_channel == 6);
    wifi_radio_channel_release(owner_b);

    set_channel_result = ESP_FAIL;
    assert(!wifi_radio_channel_reserve(owner_a, 8)); /* failed SDK retune rolls back */
    assert(wifi_radio_pinned_channel() == 6 && sdk_channel == 6);
    set_channel_result = ESP_OK;

    event_bits = CONNECTED_BIT;
    assert(wifi_radio_channel_reserve(owner_a, 6));
    assert(!wifi_radio_channel_reserve(owner_a, 8));
    assert(wifi_radio_set_channel(6)); /* no SDK retune when already there */
    assert(!wifi_radio_set_channel(8));
    assert(set_channel_calls == 2);

    event_bits = 0;
    assert(wifi_radio_pinned_channel() == 6);
    wifi_radio_channel_release(owner_a);
    assert(wifi_radio_pinned_channel() == 0);
}

static void test_common_channel_nvs_cache_and_pin_guard(void)
{
    static const char owner[4] = {'B', 'L', 'E', '1'};
    reset_wifi_stubs();
    assert(wifi_radio_common_channel() == 1);
    assert(!wifi_radio_common_channel_set(0));
    assert(!wifi_radio_common_channel_set(14));

    assert(wifi_radio_common_channel_set(6));
    assert(wifi_radio_common_channel() == 6);
    assert(nvs_set_u8_calls == 1 && nvs_commit_calls == 1);
    assert(wifi_radio_common_channel_set(6));
    assert(nvs_set_u8_calls == 1); /* unchanged value avoids a flash write */

    s_started = true;
    sdk_channel = 11;
    assert(wifi_radio_channel_reserve(owner, 11));
    assert(!wifi_radio_common_channel_set(6));
    assert(wifi_radio_common_channel_set(11));

    wifi_radio_channel_release(owner);
    nvs_commit_result = ESP_FAIL;
    assert(!wifi_radio_common_channel_set(8));
    assert(wifi_radio_common_channel() == 11); /* failed commit preserves cache */

    reset_wifi_stubs();
    nvs_get_u8_result = ESP_OK;
    nvs_channel = 13;
    known_load();
    assert(wifi_radio_common_channel() == 13);
    nvs_channel = 14;
    known_load();
    assert(wifi_radio_common_channel() == 1); /* invalid stored values use default */
}

static void test_intercom_pin_rejects_new_network_scans_but_reuses_joined_network(void)
{
    static const uint8_t owner[4] = {'O', 'A', '0', '1'};
    reset_wifi_stubs();
    s_started = true;
    s_refs = 1;
    s_radio_refs = 1;
    assert(wifi_channel_reserve(&s_channel_policy, owner, 6, false, 1));

    assert(join("network", "secret", 1000) == ESP_ERR_INVALID_STATE);
    assert(disconnect_calls == 0 && wifi_set_config_calls == 0 && wifi_connect_calls == 0);
    assert(wifi_acquire(1000) == ESP_ERR_INVALID_STATE);
    assert(s_refs == s_radio_refs && join_best_calls == 0);
    assert(scan_start_calls == 0 && disconnect_calls == 0);
    uint16_t found = 99;
    assert(scan_run(false, &found) == NULL);
    assert(found == 0 && scan_start_calls == 0 && s_scans == 0);

    event_bits = CONNECTED_BIT;
    assert(wifi_acquire(1000) == ESP_OK);
    assert(s_refs == 2 && join_best_calls == 0);
    wifi_release();
    assert(s_refs == s_radio_refs);
}

static void test_channel_snapshots_are_nonblocking_when_wifi_is_busy(void)
{
    static const char owner[4] = {'H', 'I', 'D', '1'};
    reset_wifi_stubs();
    s_started = true;
    sdk_channel = 6;
    mutex_busy = true;

    assert(!wifi_radio_set_channel(11));
    assert(!wifi_radio_channel_reserve(owner, 6));
    assert(wifi_radio_pinned_channel() == 0);
    assert(wifi_radio_common_channel() == 0);
    assert(!wifi_radio_common_channel_set(11));
    assert(get_channel_calls == 0 && set_channel_calls == 0);
    assert(wifi_channel_pinned(&s_channel_policy) == 0);

    /* Release is a lifecycle mutation and waits for the policy mutex. */
    wifi_radio_channel_release(owner);
    assert(last_mutex_wait == portMAX_DELAY);
}

int main(void)
{
    test_init_failure_destroys_owned_netifs();
    test_storage_failure_rolls_back_or_retries_cleanup();
    test_initial_mode_and_start_failures_release_driver();
    test_started_radio_survives_mode_failure();
    test_radio_off_cleans_driver_even_if_already_stopped();
    test_off_job_preserves_owners_and_retries_cleanup();
    test_off_timer_rearms_when_sys_queue_is_full();
    test_radio_only_release_disconnects_and_restores_pin();
    test_radio_only_disconnect_failures_retry_safely();
    test_radio_only_off_job_respects_new_ip_ap_and_scan_owners();
    test_release_counts_preserve_radio_holders();
    test_channel_api_obeys_shared_policy_and_network_authority();
    test_common_channel_nvs_cache_and_pin_guard();
    test_intercom_pin_rejects_new_network_scans_but_reuses_joined_network();
    test_channel_snapshots_are_nonblocking_when_wifi_is_busy();
    puts("wifi driver lifecycle tests passed");
    return 0;
}
