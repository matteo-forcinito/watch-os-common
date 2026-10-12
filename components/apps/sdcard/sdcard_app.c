#include "apps_internal.h"

#include "core/lv_port.h"
#include "core/power.h"
#include "core/sys.h"
#include "hardware/sdcard.h"
#include "services/storage.h"
#include "ui/ui.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * microSD manager: state, space and details of the card, plus the jobs a
 * watch owner needs without a PC:
 *
 *   format (FAT32)            also for cards the watch cannot read (exFAT, NTFS)
 *   create system folders     /apps, /config + examples and a README
 *   speed test                1 MB written and read back
 *   eject / mount
 *
 * Jobs are slow (a format can take a minute): they run in their own task and
 * report back with ui_async(). The page is rebuilt when the card state
 * changes (inserted, removed, formatted...).
 */

static const char *TAG = "sdcard_app";

#define TICK_MS          1000
#define JOB_STACK        4096
#define TEST_BYTES       (1024 * 1024)
#define TEST_CHUNK       (16 * 1024)
#define TEST_FILE        SDCARD_MOUNT "/.speedtest.tmp"

typedef enum
{
    JOB_NONE,
    JOB_FORMAT,
    JOB_FOLDERS,
    JOB_SPEED,
    JOB_MOUNT,
} job_t;

static lv_obj_t *s_page = NULL;
static lv_obj_t *s_space = NULL;
static lv_obj_t *s_bar = NULL;
static lv_timer_t *s_timer = NULL;
static int s_shown = -1;                /* state the page shows (sdcard_state_t, or -2 = job) */
static bool s_ejected = false;
static volatile job_t s_job = JOB_NONE;
static char s_result[96] = "";

/* ------------------------------------------------------------ helpers */

static void size_text(char *out, size_t size, uint32_t mb)
{
    if (mb >= 1024)
    {
        snprintf(out, size, "%lu,%lu GB", (unsigned long)(mb / 1024), (unsigned long)((mb % 1024) * 10 / 1024));
    }
    else
    {
        snprintf(out, size, "%lu MB", (unsigned long)mb);
    }
}

static int count_apps(void)
{
    DIR *dir = opendir(SDCARD_MOUNT "/apps");
    int count = 0;

    if (dir == NULL)
    {
        return -1;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.')
        {
            count++;
        }
    }

    closedir(dir);

    return count;
}

/* Write `text` to `path` unless the file is already there. */
static bool write_if_missing(const char *path, const char *text)
{
    struct stat st;

    if (stat(path, &st) == 0)
    {
        return true;
    }

    FILE *file = fopen(path, "w");

    if (file == NULL)
    {
        return false;
    }

    bool ok = fputs(text, file) >= 0;
    fclose(file);

    return ok;
}

static bool make_dir(const char *path)
{
    return mkdir(path, 0775) == 0 || errno == EEXIST;
}

/* --------------------------------------------------------------- jobs */

static const char s_readme[] =
    "Scheda dell'orologio\r\n"
    "====================\r\n\r\n"
    "apps/<Nome>/<Nome>.bin    app esterne (+ icon.png, manifest.json)\r\n"
    "config/wifi.txt           reti Wi-Fi: una per riga, nome = password\r\n"
    "config/radios.txt         stazioni della web radio\r\n\r\n"
    "Formato: FAT32. I file si gestiscono anche dalla pagina web dell'orologio.\r\n";

static const char s_wifi_example[] =
    "# Reti Wi-Fi conosciute: una per riga, \"nome = password\".\r\n"
    "# Le righe che iniziano con # sono ignorate. Lette all'avvio.\r\n"
    "# CasaMia = password-segreta\r\n";

static bool create_folders(void)
{
    bool ok = make_dir(SDCARD_MOUNT "/apps");
    ok &= make_dir(SDCARD_MOUNT "/config");
    ok &= write_if_missing(SDCARD_MOUNT "/LEGGIMI.txt", s_readme);
    ok &= write_if_missing(SDCARD_MOUNT "/config/wifi.txt", s_wifi_example);

    return ok;
}

/* MB/s with one decimal, as tenths. */
static int tenths_mb_s(int64_t bytes, int64_t us)
{
    return us > 0 ? (int)(bytes * 10 * 1000000 / (us * 1024 * 1024)) : 0;
}

static bool speed_test(char *out, size_t size)
{
    uint8_t *buffer = heap_caps_malloc(TEST_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (buffer == NULL)
    {
        buffer = heap_caps_malloc(TEST_CHUNK, MALLOC_CAP_SPIRAM);
    }

    if (buffer == NULL)
    {
        snprintf(out, size, "Memoria insufficiente");
        return false;
    }

    memset(buffer, 0xA5, TEST_CHUNK);

    FILE *file = fopen(TEST_FILE, "wb");
    bool ok = file != NULL;
    int64_t start = esp_timer_get_time();

    for (int done = 0; ok && done < TEST_BYTES; done += TEST_CHUNK)
    {
        ok = fwrite(buffer, 1, TEST_CHUNK, file) == TEST_CHUNK;
    }

    if (file != NULL)
    {
        fflush(file);
        fsync(fileno(file));
        fclose(file);
    }

    int64_t write_us = esp_timer_get_time() - start;

    file = ok ? fopen(TEST_FILE, "rb") : NULL;
    ok = file != NULL;
    start = esp_timer_get_time();

    for (int done = 0; ok && done < TEST_BYTES; done += TEST_CHUNK)
    {
        ok = fread(buffer, 1, TEST_CHUNK, file) == TEST_CHUNK;
    }

    int64_t read_us = esp_timer_get_time() - start;

    if (file != NULL)
    {
        fclose(file);
    }

    unlink(TEST_FILE);
    heap_caps_free(buffer);

    if (!ok)
    {
        snprintf(out, size, "Test non riuscito (scheda piena o protetta?)");
        return false;
    }

    int w = tenths_mb_s(TEST_BYTES, write_us);
    int r = tenths_mb_s(TEST_BYTES, read_us);
    snprintf(out, size, "Scrittura %d,%d MB/s - lettura %d,%d MB/s", w / 10, w % 10, r / 10, r % 10);

    return true;
}

/* lvgl task: a job finished. */
static void job_done(void *arg)
{
    (void)arg;

    s_job = JOB_NONE;
    s_shown = -1;   /* rebuild the page */
    power_keep_screen_on(false);

    if (s_result[0] != '\0')
    {
        ui_toast(s_result);
    }
}

static void job_task(void *arg)
{
    job_t job = (job_t)(intptr_t)arg;
    esp_err_t err = ESP_OK;

    switch (job)
    {
        case JOB_FORMAT:
            err = sdcard_format();

            if (err == ESP_OK)
            {
                create_folders();
            }

            snprintf(s_result, sizeof(s_result), err == ESP_OK ? "Scheda formattata (FAT32)"
                                                              : "Formattazione non riuscita: %s",
                     esp_err_to_name(err));
            break;

        case JOB_FOLDERS:
            snprintf(s_result, sizeof(s_result), "%s",
                     create_folders() ? "Cartelle di sistema pronte" : "Impossibile scrivere sulla scheda");
            break;

        case JOB_SPEED:
            speed_test(s_result, sizeof(s_result));
            break;

        case JOB_MOUNT:
            err = sdcard_mount();
            snprintf(s_result, sizeof(s_result), "%s", err == ESP_OK ? "Scheda pronta" : "Scheda non leggibile");
            break;

        default:
            break;
    }

    storage_files_release();
    ESP_LOGI(TAG, "Job %d: %s", (int)job, s_result);

    if (!ui_async(job_done, NULL))
    {
        s_job = JOB_NONE;
    }

    vTaskDelete(NULL);
}

static void start_job(job_t job)
{
    if (s_job != JOB_NONE)
    {
        return;
    }

    if (!storage_files_acquire())
    {
        ui_toast("Memoria file occupata. Attendi il download.");
        return;
    }
    s_job = job;
    s_result[0] = '\0';
    power_keep_screen_on(true);

    if (!sys_task_create(job_task, "sd_job", JOB_STACK, (void *)(intptr_t)job, 3, SYS_CORE_ANY))
    {
        s_job = JOB_NONE;
        power_keep_screen_on(false);
        storage_files_release();
        ui_toast("Memoria insufficiente");
        return;
    }

    s_shown = -1;   /* show "working" */
}

/* -------------------------------------------------------------- events */

static void on_format_confirmed(bool confirmed, void *user_data)
{
    (void)user_data;

    if (confirmed)
    {
        s_ejected = false;
        start_job(JOB_FORMAT);
    }
}

static void on_format(lv_event_t *e)
{
    (void)e;
    ui_confirm("Formattare la scheda?", "Tutti i file verranno cancellati. La scheda diventa FAT32.",
               "Formatta", UI_COLOR_RED, on_format_confirmed, NULL);
}

static void on_folders(lv_event_t *e)
{
    (void)e;
    start_job(JOB_FOLDERS);
}

static void on_speed(lv_event_t *e)
{
    (void)e;
    start_job(JOB_SPEED);
}

static void on_mount(lv_event_t *e)
{
    (void)e;
    s_ejected = false;
    start_job(JOB_MOUNT);
}

static void on_eject(lv_event_t *e)
{
    (void)e;

    if (!storage_files_acquire())
    {
        ui_toast("Memoria file occupata. Attendi il download.");
        return;
    }
    sdcard_unmount();
    storage_files_release();
    s_ejected = true;
    s_shown = -1;
    ui_toast("Puoi rimuovere la scheda");
}

/* ---------------------------------------------------------------- page */

static void build_mounted(void)
{
    sdcard_info_t info = {0};
    char size[16] = "-";
    char value[32];

    s_space = ui_text(s_page, "", false);
    lv_obj_set_style_text_align(s_space, LV_TEXT_ALIGN_CENTER, 0);

    s_bar = lv_bar_create(s_page);
    lv_obj_set_size(s_bar, LV_PCT(100), 10);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(UI_COLOR_CARD_HI), 0);
    lv_obj_set_style_bg_color(s_bar, ui_accent(), LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 1000);

    ui_section(s_page, "Dettagli");

    if (sdcard_info(&info) == ESP_OK)
    {
        size_text(size, sizeof(size), info.size_mb);
        ui_row(s_page, LV_SYMBOL_SD_CARD, UI_COLOR_BLUE, "Scheda", info.name[0] ? info.name : "-", NULL, NULL);
        snprintf(value, sizeof(value), "%s, %s", size, info.high_capacity ? "SDHC/XC" : "SD");
        ui_row(s_page, LV_SYMBOL_DRIVE, UI_COLOR_GRAY, "Tipo", value, NULL, NULL);
        snprintf(value, sizeof(value), "%lu MHz", (unsigned long)(info.freq_khz / 1000));
        ui_row(s_page, LV_SYMBOL_CHARGE, UI_COLOR_ORANGE, "Bus", value, NULL, NULL);
    }

    int apps = count_apps();
    snprintf(value, sizeof(value), "%d", apps < 0 ? 0 : apps);
    ui_row(s_page, LV_SYMBOL_LIST, UI_COLOR_INDIGO, "App esterne", value, NULL, NULL);

    ui_section(s_page, "Azioni");
    ui_row(s_page, LV_SYMBOL_DIRECTORY, UI_COLOR_TEAL, "Crea cartelle di sistema", NULL, on_folders, NULL);
    ui_row(s_page, LV_SYMBOL_REFRESH, UI_COLOR_GREEN, "Test velocità", NULL, on_speed, NULL);
    ui_row(s_page, LV_SYMBOL_EJECT, UI_COLOR_GRAY, "Espelli", NULL, on_eject, NULL);
    ui_row(s_page, LV_SYMBOL_TRASH, UI_COLOR_RED, "Formatta", NULL, on_format, NULL);

    ui_text(s_page, "I file si gestiscono anche dall'app Pagina web, dal browser.", true);
}

static void update_space(void)
{
    if (s_space == NULL)
    {
        return;
    }

    uint32_t total_mb = 0;
    uint32_t free_mb = 0;
    char total[16];
    char free_text[16];

    sdcard_space(&total_mb, &free_mb);
    size_text(total, sizeof(total), total_mb);
    size_text(free_text, sizeof(free_text), free_mb);
    lv_label_set_text_fmt(s_space, "%s liberi su %s", free_text, total);
    lv_bar_set_value(s_bar, total_mb > 0 ? (int32_t)((uint64_t)(total_mb - free_mb) * 1000 / total_mb) : 0,
                     LV_ANIM_OFF);
}

static void rebuild(int state)
{
    /* Keep the title (first child), drop the rest. */
    while (lv_obj_get_child_count(s_page) > 1)
    {
        lv_obj_delete(lv_obj_get_child(s_page, 1));
    }

    s_space = NULL;
    s_bar = NULL;

    if (s_job != JOB_NONE)
    {
        lv_obj_t *spinner = lv_spinner_create(s_page);
        lv_obj_set_size(spinner, 64, 64);
        lv_obj_set_style_arc_color(spinner, ui_accent(), LV_PART_INDICATOR);
        ui_text(s_page, s_job == JOB_FORMAT ? "Formattazione in corso...\nNon togliere la scheda."
                                            : "Attendi...", false);
        return;
    }

    switch (state)
    {
        case SDCARD_MOUNTED:
            build_mounted();
            update_space();
            break;

        case SDCARD_UNREADABLE:
            ui_text(s_page, "La scheda risponde ma il suo formato non è leggibile "
                            "(exFAT o NTFS?). Formattala in FAT32 per usarla.", false);
            ui_button(s_page, "Formatta FAT32", UI_COLOR_RED, on_format, NULL);
            break;

        case SDCARD_BUSY:
            ui_text(s_page, "La scheda è in uso come unità USB.", false);
            break;

        default:
            ui_text(s_page, s_ejected ? "Scheda espulsa: puoi rimuoverla." : "Nessuna scheda microSD.", false);
            ui_button(s_page, s_ejected ? "Monta di nuovo" : "Riprova", UI_COLOR_BLUE, on_mount, NULL);
            break;
    }
}

static void tick(lv_timer_t *timer)
{
    (void)timer;

    int state = s_job != JOB_NONE ? -2 : (int)sdcard_state();

    if (state == SDCARD_MOUNTED)
    {
        s_ejected = false;
    }

    if (state != s_shown)
    {
        s_shown = state;
        rebuild(state);
    }
    else if (state == SDCARD_MOUNTED)
    {
        update_space();
    }
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;

    s_page = ui_page(screen, "Scheda SD");
    s_shown = -1;
    s_timer = lv_timer_create(tick, TICK_MS, NULL);
    tick(s_timer);
}

static void destroy(void)
{
    lv_timer_delete(s_timer);
    s_timer = NULL;
    s_page = NULL;
    s_space = NULL;
    s_bar = NULL;
    /* A running job finishes by itself and reports with a toast. */
}

const app_t sdcard_app = {
    .id = "sdcard",
    .name = "Scheda SD",
    .icon = LV_SYMBOL_SD_CARD,
    .color = UI_COLOR_TEAL,
    .create = create,
    .destroy = destroy,
};
