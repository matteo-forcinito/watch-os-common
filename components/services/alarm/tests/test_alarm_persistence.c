#include <stdio.h>

#define CHECK(condition, message) \
    do \
    { \
        if (!(condition)) \
        { \
            ++failures; \
            fprintf(stderr, "FAIL: %s\n", message); \
        } \
    } while (0)

static int failures;

static alarm_t baseline_alarm(void)
{
    alarm_t alarm = {0};
    alarm.id = 12;
    alarm.hour = 6;
    alarm.minute = 45;
    alarm.days = ALARM_DAYS_WEEKDAYS;
    alarm.enabled = true;
    alarm.radio = -1;
    memcpy(alarm.label, "Morning", sizeof("Morning"));
    return alarm;
}

static void seed_one_alarm(void)
{
    s_alarms[0] = baseline_alarm();
    s_count = 1;
    memcpy(s_blob, s_alarms, sizeof(alarm_t));
    s_blob_size = sizeof(alarm_t);
}

static alarm_t edited_alarm(void)
{
    alarm_t alarm = baseline_alarm();
    alarm.hour = 8;
    memcpy(alarm.label, "Updated", sizeof("Updated"));
    return alarm;
}

static void check_save_failure(int open_result, int set_blob_result, int commit_result,
                               const char *case_name)
{
    reset_alarm_mocks();
    seed_one_alarm();
    alarm_t before = s_alarms[0];
    alarm_t candidate = edited_alarm();
    s_open_result = open_result;
    s_set_blob_result = set_blob_result;
    s_commit_result = commit_result;

    esp_err_t result = alarm_save(&candidate);
    CHECK(result != ESP_OK, case_name);
    CHECK(s_count == 1 && memcmp(&s_alarms[0], &before, sizeof(before)) == 0,
          "failed alarm_save must preserve the runtime record");
    CHECK(candidate.id == before.id, "failed update must preserve the caller's existing ID");
    CHECK(s_state_bumps == 0, "failed alarm_save must not bump alarm version");
    CHECK(s_lock_depth == 0, "alarm_save must release its lock on storage failure");
}

static void test_save_failures_are_reported_and_rolled_back(void)
{
    check_save_failure(ESP_FAIL, ESP_OK, ESP_OK, "alarm_save reports NVS open failure");
    check_save_failure(ESP_OK, ESP_FAIL, ESP_OK, "alarm_save reports NVS set_blob failure");
    check_save_failure(ESP_OK, ESP_OK, ESP_FAIL, "alarm_save reports NVS commit failure");
}

static void check_delete_failure(int open_result, int set_blob_result, int commit_result,
                                 const char *case_name)
{
    reset_alarm_mocks();
    seed_one_alarm();
    alarm_t before = s_alarms[0];
    s_open_result = open_result;
    s_set_blob_result = set_blob_result;
    s_commit_result = commit_result;

    esp_err_t result = alarm_delete(before.id);
    CHECK(result != ESP_OK, case_name);
    CHECK(s_count == 1 && memcmp(&s_alarms[0], &before, sizeof(before)) == 0,
          "failed alarm_delete must preserve the runtime list");
    CHECK(s_state_bumps == 0, "failed alarm_delete must not bump alarm version");
    CHECK(s_lock_depth == 0, "alarm_delete must release its lock on storage failure");
}

static void test_delete_failures_are_reported_and_rolled_back(void)
{
    check_delete_failure(ESP_FAIL, ESP_OK, ESP_OK, "alarm_delete reports NVS open failure");
    check_delete_failure(ESP_OK, ESP_FAIL, ESP_OK, "alarm_delete reports NVS set_blob failure");
    check_delete_failure(ESP_OK, ESP_OK, ESP_FAIL, "alarm_delete reports NVS commit failure");
}

static void test_failed_create_keeps_zero_id_and_old_list(void)
{
    reset_alarm_mocks();
    seed_one_alarm();
    alarm_t before = s_alarms[0];
    alarm_t candidate = {0};
    candidate.hour = 9;
    candidate.minute = 5;
    candidate.days = ALARM_DAYS_ONCE;
    candidate.enabled = true;
    candidate.radio = -1;
    memcpy(candidate.label, "New", sizeof("New"));
    s_commit_result = ESP_FAIL;

    CHECK(alarm_save(&candidate) != ESP_OK, "failed create must report storage failure");
    CHECK(candidate.id == 0, "failed create must leave the caller's ID unset");
    CHECK(s_count == 1 && memcmp(&s_alarms[0], &before, sizeof(before)) == 0,
          "failed create must preserve the previous runtime list");
    CHECK(s_state_bumps == 0, "failed create must not bump alarm version");
}

static void test_load_rejects_malformed_blob_lengths(void)
{
    reset_alarm_mocks();
    s_imported = 1;
    s_get_blob_result = ESP_OK;
    s_blob_size = sizeof(alarm_t) + 1;
    load();
    CHECK(s_count == 0, "load rejects a blob whose byte length is not a record multiple");

    reset_alarm_mocks();
    s_imported = 1;
    s_get_blob_result = ESP_OK;
    s_blob_size = sizeof(s_alarms) + sizeof(alarm_t);
    load();
    CHECK(s_count == 0, "load rejects a blob reporting more than ALARM_MAX records");
}

static void test_import_path_remains_available(void)
{
    reset_alarm_mocks();
    s_imported = 0;
    s_sd_mounted = true;
    s_get_blob_result = ESP_FAIL;
    load();
    CHECK(s_import_calls == 1 && s_count == 1,
          "missing imported marker still invokes the Arduino alarm import");
    CHECK(s_alarms[0].id == 71 && strcmp(s_alarms[0].label, "Imported") == 0,
          "imported local alarm is retained");
    CHECK(s_nvs_set_u8_calls == 1 && s_nvs_commit_calls >= 1,
          "import marker and imported alarms are saved");
}

static void test_timer_post_failure_rearms_existing_timer(void)
{
    reset_alarm_mocks();
    s_post_result = 0;
    timer_cb(NULL);
    CHECK(s_post_calls == 1, "timer callback posts the alarm check to sys worker");
    CHECK(s_timer_starts > 0 && s_timer_delay > 0,
          "failed sys_post rearms the existing timer instead of losing the alarm check");

    reset_alarm_mocks();
    timer_cb(NULL);
    CHECK(s_post_calls == 1 && s_timer_starts == 0,
          "successful sys_post leaves scheduling to the check job");
}

static void test_successful_mutations_and_middle_rollback(void)
{
    reset_alarm_mocks();
    seed_one_alarm();
    alarm_t a = edited_alarm();
    CHECK(alarm_save(&a) == ESP_OK && s_state_bumps == 1,
          "successful update persists and bumps version");
    a.id = 0;
    CHECK(alarm_save(&a) == ESP_OK && a.id == 13 && s_count == 2,
          "successful create assigns ID only after commit");
    a.id = 0;
    CHECK(alarm_save(&a) == ESP_OK && a.id == 14 && s_count == 3,
          "third alarm created");
    alarm_t before[3];
    memcpy(before, s_alarms, sizeof(before));
    s_commit_result = ESP_FAIL;
    CHECK(alarm_delete(13) != ESP_OK && s_count == 3 &&
          memcmp(before, s_alarms, sizeof(before)) == 0,
          "failed middle deletion restores all following records");
    s_commit_result = ESP_OK;
    CHECK(alarm_delete(13) == ESP_OK && s_count == 2 && s_alarms[1].id == 14,
          "successful middle deletion retains following record");
}

static void test_import_failure_does_not_publish_alarms(void)
{
    reset_alarm_mocks();
    s_imported = 0;
    s_sd_mounted = true;
    s_commit_result = ESP_FAIL;
    load();
    CHECK(s_count == 0 && s_state_bumps == 0,
          "failed import commit does not publish unpersisted alarms");
    CHECK(s_nvs_commit_calls == 1,
          "import records and marker share one commit");
}

static void test_load_preserves_valid_records_and_rejects_invalid_ids(void)
{
    reset_alarm_mocks();
    seed_one_alarm();
    s_get_blob_result = ESP_OK;
    load();
    CHECK(s_count == 1 && s_alarms[0].id == 12,
          "load preserves valid historical local alarms");

    alarm_t invalid = baseline_alarm();
    invalid.hour = 24;
    memcpy(s_blob, &invalid, sizeof(invalid));
    load();
    CHECK(s_count == 0, "load rejects invalid persisted alarm time");

    invalid = baseline_alarm();
    memset(invalid.label, 'x', sizeof(invalid.label));
    memcpy(s_blob, &invalid, sizeof(invalid));
    load();
    CHECK(s_count == 0, "load rejects unterminated persisted labels");

    invalid = baseline_alarm();
    memcpy(s_blob, &invalid, sizeof(invalid));
    memcpy(s_blob + sizeof(invalid), &invalid, sizeof(invalid));
    s_blob_size = 2 * sizeof(invalid);
    load();
    CHECK(s_count == 0, "load rejects duplicate persisted identifiers");
}

int main(void)
{
    test_load_preserves_valid_records_and_rejects_invalid_ids();
    test_successful_mutations_and_middle_rollback();
    test_import_failure_does_not_publish_alarms();
    test_save_failures_are_reported_and_rolled_back();
    test_delete_failures_are_reported_and_rolled_back();
    test_failed_create_keeps_zero_id_and_old_list();
    test_load_rejects_malformed_blob_lengths();
    test_import_path_remains_available();
    test_timer_post_failure_rearms_existing_timer();
    if (failures != 0)
    {
        fprintf(stderr, "%d alarm persistence contract checks failed\n", failures);
        return 1;
    }
    puts("Alarm persistence contract tests passed");
    return 0;
}
