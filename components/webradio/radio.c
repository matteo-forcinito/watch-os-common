#include "webradio/radio.h"

#include "radio_list.h"
#include "core/sys.h"

#include "core/settings.h"
#include "core/state.h"
#include "hardware/sdcard.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *TAG = "radio_svc";

#define LIST_FILE       SDCARD_MOUNT "/config/radios.txt"
#define NVS_NAMESPACE   "radio"
#define NVS_FAVORITES   "favorites"
#define FAV_NAME_MAX    RADIO_NAME_MAX
#define DEFAULT_FAVORITES 6

/* components/services/radio/radios_default.txt (EMBED_TXTFILES, NUL-terminated). */
extern const char radios_default_start[] asm("_binary_radios_default_txt_start");

typedef enum
{
    CMD_PLAY,
    CMD_STOP,
} cmd_type_t;

typedef struct
{
    cmd_type_t type;
    int index;
} cmd_t;

static radio_list_t s_list;
static SemaphoreHandle_t s_mutex = NULL;
static QueueHandle_t s_queue = NULL;
static char s_favorites[RADIO_FAVORITES_MAX][FAV_NAME_MAX];
static int s_favorite_count = 0;
static volatile int s_current = -1;
static volatile int s_last = -1;

/* ------------------------------------------------------------ storage */

static void favorites_save(void)
{
    nvs_handle_t nvs;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK)
    {
        nvs_set_blob(nvs, NVS_FAVORITES, s_favorites, (size_t)s_favorite_count * FAV_NAME_MAX);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void favorites_load(void)
{
    nvs_handle_t nvs;
    s_favorite_count = 0;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK)
    {
        size_t size = sizeof(s_favorites);

        if (nvs_get_blob(nvs, NVS_FAVORITES, s_favorites, &size) == ESP_OK)
        {
            s_favorite_count = (int)(size / FAV_NAME_MAX);
            nvs_close(nvs);
            return;
        }

        nvs_close(nvs);
    }

    /* First start: the first stations of the built-in list. */
    radio_list_t defaults;

    if (radio_list_init(&defaults))
    {
        radio_list_parse_text(&defaults, radios_default_start);

        for (int i = 0; i < defaults.count && i < DEFAULT_FAVORITES; i++)
        {
            snprintf(s_favorites[s_favorite_count++], FAV_NAME_MAX, "%s", defaults.stations[i].name);
        }

        radio_list_free(&defaults);
    }
}

static void list_load(void)
{
    s_list.count = 0;

    FILE *file = sdcard_is_mounted() ? fopen(LIST_FILE, "r") : NULL;

    if (file != NULL)
    {
        radio_list_read(&s_list, file);
        fclose(file);
        ESP_LOGI(TAG, "%d stations from the SD card", s_list.count);
    }

    if (s_list.count == 0)
    {
        radio_list_parse_text(&s_list, radios_default_start);
        ESP_LOGI(TAG, "%d built-in stations", s_list.count);
    }
}

static esp_err_t list_save(void)
{
    if (!sdcard_is_mounted())
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (mkdir(SDCARD_MOUNT "/config", 0775) != 0 && errno != EEXIST)
    {
        return ESP_FAIL;
    }

    FILE *file = fopen(LIST_FILE, "w");

    if (file == NULL)
    {
        return ESP_FAIL;
    }

    radio_list_write(&s_list, file);
    fclose(file);

    return ESP_OK;
}

/* ------------------------------------------------------------- worker */

static void worker_task(void *arg)
{
    (void)arg;

    cmd_t cmd;

    while (xQueueReceive(s_queue, &cmd, portMAX_DELAY) == pdTRUE)
    {
        /* Only the newest command matters (fast "next next next"). */
        cmd_t newer;

        while (xQueueReceive(s_queue, &newer, 0) == pdTRUE)
        {
            cmd = newer;
        }

        radio_player_stop();

        if (cmd.type == CMD_STOP)
        {
            s_current = -1;
            state_bump(STATE_RADIO_VERSION);
            continue;
        }

        radio_info_t info;

        if (!radio_get(cmd.index, &info))
        {
            continue;
        }

        /* A previous stream may still be closing (slow server): wait for it. */
        for (int i = 0; i < 100 && radio_player_busy(); i++)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        s_current = cmd.index;
        s_last = cmd.index;

        settings_t s = *settings_get();
        if (s.radio_station != cmd.index)
        {
            s.radio_station = (int16_t)cmd.index;
            settings_save(&s);
        }

        state_bump(STATE_RADIO_VERSION);
        radio_player_start(info.url, settings_get()->volume, true);
    }
}

/* ------------------------------------------------------------- public */

void radio_service_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(4, sizeof(cmd_t));

    radio_list_init(&s_list);
    list_load();
    favorites_load();

    s_last = settings_get()->radio_station;

    if (s_last >= s_list.count)
    {
        s_last = -1;
    }

    sys_task_create(worker_task, "radio_ctl", 4096, NULL, 4, 0);
}

int radio_count(void)
{
    return s_list.count;
}

bool radio_get(int index, radio_info_t *out)
{
    bool ok = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (index >= 0 && index < s_list.count)
    {
        const radio_station_t *st = &s_list.stations[index];
        snprintf(out->name, sizeof(out->name), "%s", st->name);
        snprintf(out->genre, sizeof(out->genre), "%s", st->genre);
        snprintf(out->url, sizeof(out->url), "%s", st->url);
        ok = true;
    }

    xSemaphoreGive(s_mutex);

    return ok;
}

int radio_favorites(int *indexes, int max)
{
    int count = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int f = 0; f < s_favorite_count && count < max; f++)
    {
        int index = radio_list_find(&s_list, s_favorites[f]);

        if (index >= 0)
        {
            indexes[count++] = index;
        }
    }

    xSemaphoreGive(s_mutex);

    return count;
}

bool radio_is_favorite(int index)
{
    bool found = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (index >= 0 && index < s_list.count)
    {
        for (int f = 0; f < s_favorite_count && !found; f++)
        {
            found = strcasecmp(s_favorites[f], s_list.stations[index].name) == 0;
        }
    }

    xSemaphoreGive(s_mutex);

    return found;
}

void radio_set_favorite(int index, bool favorite)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (index < 0 || index >= s_list.count)
    {
        xSemaphoreGive(s_mutex);
        return;
    }

    const char *name = s_list.stations[index].name;
    int found = -1;

    for (int f = 0; f < s_favorite_count; f++)
    {
        if (strcasecmp(s_favorites[f], name) == 0)
        {
            found = f;
        }
    }

    if (favorite && found < 0 && s_favorite_count < RADIO_FAVORITES_MAX)
    {
        snprintf(s_favorites[s_favorite_count++], FAV_NAME_MAX, "%s", name);
    }
    else if (!favorite && found >= 0)
    {
        memmove(s_favorites[found], s_favorites[found + 1], (size_t)(s_favorite_count - found - 1) * FAV_NAME_MAX);
        s_favorite_count--;
    }

    favorites_save();
    xSemaphoreGive(s_mutex);

    state_bump(STATE_RADIO_VERSION);
}

void radio_play(int index)
{
    cmd_t cmd = {.type = CMD_PLAY, .index = index};
    s_current = index;
    xQueueSend(s_queue, &cmd, 0);
}

void radio_stop(void)
{
    cmd_t cmd = {.type = CMD_STOP, .index = -1};
    xQueueSend(s_queue, &cmd, 0);
}

void radio_toggle(void)
{
    if (radio_is_on())
    {
        radio_stop();
    }
    else
    {
        int favorites[1];
        int index = s_last >= 0 ? s_last : (radio_favorites(favorites, 1) > 0 ? favorites[0] : 0);
        radio_play(index);
    }
}

void radio_next(int direction)
{
    int base = s_current >= 0 ? s_current : s_last;
    int favorites[RADIO_FAVORITES_MAX];
    int count = radio_favorites(favorites, RADIO_FAVORITES_MAX);

    /* Inside the favorites when the current station is one of them. */
    for (int i = 0; i < count; i++)
    {
        if (favorites[i] == base)
        {
            radio_play(favorites[(i + direction + count) % count]);
            return;
        }
    }

    if (s_list.count > 0)
    {
        radio_play(((base < 0 ? 0 : base) + direction + s_list.count) % s_list.count);
    }
}

int radio_current(void)
{
    return s_current;
}

int radio_last(void)
{
    return s_current >= 0 ? s_current : s_last;
}

bool radio_is_on(void)
{
    radio_state_t st = radio_player_state();
    return s_current >= 0 && st != RADIO_STATE_IDLE && st != RADIO_STATE_FAILED;
}

void radio_set_volume(int percent)
{
    radio_player_set_volume(percent);

    settings_t s = *settings_get();
    s.volume = (uint8_t)percent;
    settings_save(&s);
}

void radio_reload(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    list_load();
    xSemaphoreGive(s_mutex);
    state_bump(STATE_RADIO_VERSION);
}

esp_err_t radio_add(const char *name, const char *genre, const char *url)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = radio_list_add(&s_list, name, genre, url);
    esp_err_t err = ok ? list_save() : ESP_ERR_INVALID_ARG;
    xSemaphoreGive(s_mutex);
    state_bump(STATE_RADIO_VERSION);

    return err;
}

esp_err_t radio_remove(int index)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = radio_list_remove(&s_list, index);
    esp_err_t err = ok ? list_save() : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(s_mutex);
    state_bump(STATE_RADIO_VERSION);

    return err;
}

char *radio_export_text(void)
{
    size_t size = 1024 + (size_t)s_list.count * (RADIO_NAME_MAX + RADIO_GENRE_MAX + RADIO_URL_MAX + 8);
    char *text = malloc(size);

    if (text == NULL)
    {
        return NULL;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    FILE *mem = fmemopen(text, size, "w");

    if (mem != NULL)
    {
        radio_list_write(&s_list, mem);
        fclose(mem);
    }
    else
    {
        text[0] = '\0';
    }

    xSemaphoreGive(s_mutex);

    return text;
}
