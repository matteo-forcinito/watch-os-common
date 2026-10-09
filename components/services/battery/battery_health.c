#include "services/battery_health.h"

#include "core/clock.h"
#include "core/state.h"
#include "core/sys.h"
#include "hardware/pmu.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include <string.h>
#include <time.h>

static const char *TAG = "battery";

#define SAMPLE_US        (120LL * 1000 * 1000)   /* every 2 minutes */
#define NVS_NAMESPACE    "battery"
#define NVS_KEY          "health"
#define STORE_VERSION    1
#define SAVE_EVERY_PCT   5      /* write to flash after this much more use */
#define DROP_HYSTERESIS  2      /* % readings wobble: count drops of at least this */
#define RISE_RESET       3      /* risen this much without a cable: re-read the level */
#define RATE_SAMPLES     32     /* ~1 hour of samples for the time left */
#define RATE_MIN_MINUTES 20
#define RATE_MIN_DROP    3
#define MIN_TEST_DROP_MV 3

/* What survives a restart. */
typedef struct
{
    uint32_t version;
    int64_t first_seen;      /* unix time, 0 = clock never valid yet */
    uint32_t used_percent;   /* total % used while on battery: 100 = one cycle */
    int32_t first_drop_mv;   /* resistance test: first result on this battery */
    int32_t last_drop_mv;
    uint32_t tests;
} store_t;

typedef struct
{
    int64_t when_us;
    int percent;
} sample_t;

static SemaphoreHandle_t s_lock = NULL;
static esp_timer_handle_t s_timer = NULL;
static store_t s_store;
static uint32_t s_saved_used = 0;
static int s_reference = BATTERY_UNKNOWN;   /* level the next drop is counted from */
static int s_mv = 0;
static sample_t s_rate[RATE_SAMPLES];
static int s_rate_count = 0;

/* ------------------------------------------------------------ storage */

static void load(void)
{
    nvs_handle_t nvs;
    size_t size = sizeof(s_store);

    memset(&s_store, 0, sizeof(s_store));

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK)
    {
        if (nvs_get_blob(nvs, NVS_KEY, &s_store, &size) != ESP_OK || s_store.version != STORE_VERSION)
        {
            memset(&s_store, 0, sizeof(s_store));
        }

        nvs_close(nvs);
    }

    s_store.version = STORE_VERSION;
    s_saved_used = s_store.used_percent;
}

/* sys worker or a task: never from an esp_timer callback. */
static void save(void)
{
    nvs_handle_t nvs;
    store_t copy;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy = s_store;
    s_saved_used = s_store.used_percent;
    xSemaphoreGive(s_lock);

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_blob(nvs, NVS_KEY, &copy, sizeof(copy));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* ------------------------------------------------------------ sampling */

static void rate_reset(void)
{
    s_rate_count = 0;
}

static void rate_add(int percent)
{
    if (s_rate_count == RATE_SAMPLES)
    {
        memmove(&s_rate[0], &s_rate[1], sizeof(sample_t) * (RATE_SAMPLES - 1));
        s_rate_count--;
    }

    s_rate[s_rate_count++] = (sample_t){esp_timer_get_time(), percent};
}

/* Minutes left at the recent pace, or BATTERY_UNKNOWN while there is too little history. */
static int minutes_left(int percent)
{
    if (s_rate_count < 2 || percent <= 0)
    {
        return BATTERY_UNKNOWN;
    }

    const sample_t *first = &s_rate[0];
    const sample_t *last = &s_rate[s_rate_count - 1];
    int64_t minutes = (last->when_us - first->when_us) / (60LL * 1000 * 1000);
    int drop = first->percent - last->percent;

    if (minutes < RATE_MIN_MINUTES || drop < RATE_MIN_DROP)
    {
        return BATTERY_UNKNOWN;
    }

    return (int)((int64_t)percent * minutes / drop);
}

static void sample_job(void *arg)
{
    (void)arg;

    int percent = state_get(STATE_BATTERY);
    bool on_cable = state_get(STATE_CHARGING) != 0 || state_get(STATE_USB) != 0;
    bool save_now = false;
    int mv = pmu_battery_mv();

    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_mv = mv;

    if (s_store.first_seen == 0 && clock_is_valid())
    {
        s_store.first_seen = (int64_t)time(NULL);
        save_now = true;
    }

    if (percent >= 0)
    {
        if (on_cable)
        {
            s_reference = percent;
            rate_reset();
        }
        else
        {
            if (s_reference < 0 || percent > s_reference + RISE_RESET)
            {
                s_reference = percent;
            }
            else if (percent <= s_reference - DROP_HYSTERESIS)
            {
                s_store.used_percent += (uint32_t)(s_reference - percent);
                s_reference = percent;
            }

            rate_add(percent);
        }

        save_now |= s_store.used_percent >= s_saved_used + SAVE_EVERY_PCT;
    }

    xSemaphoreGive(s_lock);

    if (save_now)
    {
        save();
    }
}

/* esp_timer task: only hand the work to the sys worker. */
static void sample_timer_cb(void *arg)
{
    (void)arg;
    sys_post(sample_job, NULL);
}

/* ------------------------------------------------------------- public */

void battery_health_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    if (s_lock == NULL)
    {
        return;
    }

    load();

    const esp_timer_create_args_t args = {.callback = sample_timer_cb, .name = "battery"};

    if (esp_timer_create(&args, &s_timer) == ESP_OK)
    {
        esp_timer_start_periodic(s_timer, SAMPLE_US);
    }

    ESP_LOGI(TAG, "%lu.%lu cycles, %lu tests", (unsigned long)(s_store.used_percent / 100),
             (unsigned long)(s_store.used_percent % 100 / 10), (unsigned long)s_store.tests);
}

void battery_health_get(battery_report_t *out)
{
    memset(out, 0, sizeof(*out));

    out->percent = state_get(STATE_BATTERY);
    out->charging = state_get(STATE_CHARGING) != 0;
    out->usb = state_get(STATE_USB) != 0;
    out->full = out->usb && !out->charging && out->percent >= 95;

    if (s_lock == NULL)
    {
        out->health = out->health_cycles = out->health_age = out->health_resistance = BATTERY_UNKNOWN;
        out->days = out->minutes_left = BATTERY_UNKNOWN;
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    out->mv = s_mv;
    out->cycles_x10 = s_store.used_percent / 10;
    out->first_seen = s_store.first_seen;
    out->tests = (int)s_store.tests;
    out->minutes_left = out->usb || out->charging ? BATTERY_UNKNOWN : minutes_left(out->percent);

    int first_drop = (int)s_store.first_drop_mv;
    int last_drop = (int)s_store.last_drop_mv;

    xSemaphoreGive(s_lock);

    out->days = BATTERY_UNKNOWN;

    if (out->first_seen > 0 && clock_is_valid())
    {
        int64_t seconds = (int64_t)time(NULL) - out->first_seen;
        out->days = seconds > 0 ? (int)(seconds / 86400) : 0;
    }

    out->health_cycles = battery_health_from_cycles(out->cycles_x10);
    out->health_age = battery_health_from_age(out->days);
    out->health_resistance = out->tests >= 2 ? battery_health_from_resistance(first_drop, last_drop)
                                             : BATTERY_UNKNOWN;
    out->resistance_change = (out->tests >= 2 && first_drop > 0) ? (last_drop - first_drop) * 100 / first_drop : 0;

    /* The worst indicator we have. */
    out->health = out->health_cycles;

    if (out->health_age != BATTERY_UNKNOWN && out->health_age < out->health)
    {
        out->health = out->health_age;
    }

    if (out->health_resistance != BATTERY_UNKNOWN && out->health_resistance < out->health)
    {
        out->health = out->health_resistance;
    }

    if (out->percent < 0)
    {
        out->health = BATTERY_UNKNOWN;   /* no battery */
    }
}

esp_err_t battery_health_add_test(int idle_mv, int load_mv, int percent)
{
    int drop = idle_mv - load_mv;

    ESP_LOGI(TAG, "Health test at %d%%: %d mV -> %d mV (drop %d mV)", percent, idle_mv, load_mv, drop);

    if (s_lock == NULL || idle_mv <= 0 || drop < MIN_TEST_DROP_MV)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_store.tests == 0)
    {
        s_store.first_drop_mv = drop;
    }

    s_store.last_drop_mv = drop;
    s_store.tests++;

    xSemaphoreGive(s_lock);

    save();

    return ESP_OK;
}

void battery_health_reset(void)
{
    if (s_lock == NULL)
    {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_store, 0, sizeof(s_store));
    s_store.version = STORE_VERSION;
    s_store.first_seen = clock_is_valid() ? (int64_t)time(NULL) : 0;
    s_reference = BATTERY_UNKNOWN;
    rate_reset();
    xSemaphoreGive(s_lock);

    save();
    ESP_LOGI(TAG, "History reset (new battery)");
}
