#ifndef ALARM_TEST_STUBS_H
#define ALARM_TEST_STUBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "services/alarm.h"

#define NVS_NAMESPACE "alarms"
#define NVS_KEY "list"
#define NVS_IMPORTED "imported"
#define ALARM_TEST_BLOB_CAP 4096

typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1

static alarm_t s_alarms[ALARM_MAX];
static int s_count;
static int s_lock_depth;
static int s_state_bumps;
static int s_reschedule_calls;
static int s_timer_starts;
static int s_timer_stops;
static int s_post_calls;
static int s_import_calls;
static int s_nvs_open_calls;
static int s_nvs_get_blob_calls;
static int s_nvs_get_u8_calls;
static int s_nvs_set_blob_calls;
static int s_nvs_set_u8_calls;
static int s_nvs_commit_calls;
static int s_nvs_close_calls;
static int s_open_result;
static int s_get_blob_result;
static int s_get_u8_result;
static int s_set_blob_result;
static int s_set_u8_result;
static int s_commit_result;
static int s_post_result;
static int s_timer_result;
static bool s_sd_mounted;
static uint8_t s_imported;
static uint8_t s_blob[ALARM_TEST_BLOB_CAP];
static size_t s_blob_size;
static uint8_t s_staged_blob[ALARM_TEST_BLOB_CAP];
static size_t s_staged_blob_size;
static uint64_t s_timer_delay;
static int s_timer;

static void lock(void)
{
    ++s_lock_depth;
}

static void unlock(void)
{
    --s_lock_depth;
}

static void reschedule(void)
{
    ++s_reschedule_calls;
}

static bool sdcard_is_mounted(void)
{
    return s_sd_mounted;
}

static void import_arduino(void)
{
    ++s_import_calls;
    if (s_count < ALARM_MAX)
    {
        alarm_t alarm = {0};
        alarm.id = 71;
        alarm.hour = 7;
        alarm.minute = 15;
        alarm.days = ALARM_DAYS_EVERYDAY;
        alarm.enabled = true;
        alarm.radio = -1;
        memcpy(alarm.label, "Imported", sizeof("Imported"));
        s_alarms[s_count++] = alarm;
    }
}

static void state_bump(int state)
{
    (void)state;
    ++s_state_bumps;
}

static int nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    (void)name;
    (void)mode;
    ++s_nvs_open_calls;
    if (s_open_result == ESP_OK)
    {
        *handle = 1;
    }
    return s_open_result;
}

static int nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    (void)handle;
    (void)key;
    ++s_nvs_get_blob_calls;
    if (s_get_blob_result != ESP_OK)
    {
        return s_get_blob_result;
    }
    size_t capacity = *size;
    *size = s_blob_size;
    if (s_blob_size > capacity)
    {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    if (s_blob_size > 0)
    {
        memcpy(out, s_blob, s_blob_size);
    }
    return ESP_OK;
}

static int nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    (void)handle;
    (void)key;
    ++s_nvs_get_u8_calls;
    if (s_get_u8_result == ESP_OK)
    {
        *value = s_imported;
    }
    return s_get_u8_result;
}

static int nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    (void)handle;
    (void)key;
    ++s_nvs_set_blob_calls;
    if (s_set_blob_result == ESP_OK && size <= sizeof(s_staged_blob))
    {
        memcpy(s_staged_blob, data, size);
        s_staged_blob_size = size;
    }
    return s_set_blob_result;
}

static int nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    (void)key;
    ++s_nvs_set_u8_calls;
    if (s_set_u8_result == ESP_OK)
    {
        s_imported = value;
    }
    return s_set_u8_result;
}

static int nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    ++s_nvs_commit_calls;
    if (s_commit_result == ESP_OK)
    {
        memcpy(s_blob, s_staged_blob, s_staged_blob_size);
        s_blob_size = s_staged_blob_size;
    }
    return s_commit_result;
}

static void nvs_close(nvs_handle_t handle)
{
    (void)handle;
    ++s_nvs_close_calls;
}

static bool sys_post(void (*fn)(void *), void *arg)
{
    (void)fn;
    (void)arg;
    ++s_post_calls;
    return s_post_result != 0;
}

static int esp_timer_start_once(int timer, uint64_t delay)
{
    (void)timer;
    ++s_timer_starts;
    s_timer_delay = delay;
    return s_timer_result;
}

static int esp_timer_stop(int timer)
{
    (void)timer;
    ++s_timer_stops;
    return ESP_OK;
}

static void reset_alarm_mocks(void)
{
    memset(s_alarms, 0, sizeof(s_alarms));
    memset(s_blob, 0, sizeof(s_blob));
    memset(s_staged_blob, 0, sizeof(s_staged_blob));
    s_count = 0;
    s_lock_depth = 0;
    s_state_bumps = 0;
    s_reschedule_calls = 0;
    s_timer_starts = 0;
    s_timer_stops = 0;
    s_post_calls = 0;
    s_import_calls = 0;
    s_nvs_open_calls = 0;
    s_nvs_get_blob_calls = 0;
    s_nvs_get_u8_calls = 0;
    s_nvs_set_blob_calls = 0;
    s_nvs_set_u8_calls = 0;
    s_nvs_commit_calls = 0;
    s_nvs_close_calls = 0;
    s_open_result = ESP_OK;
    s_get_blob_result = ESP_FAIL;
    s_get_u8_result = ESP_OK;
    s_set_blob_result = ESP_OK;
    s_set_u8_result = ESP_OK;
    s_commit_result = ESP_OK;
    s_post_result = 1;
    s_timer_result = ESP_OK;
    s_sd_mounted = false;
    s_imported = 1;
    s_blob_size = 0;
    s_staged_blob_size = 0;
    s_timer_delay = 0;
    s_timer = 1;
}

#endif
