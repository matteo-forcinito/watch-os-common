/*
 * Sistema: the state of the device, and a light overlay to watch it under load.
 *
 *   App      internal RAM, PSRAM, flash file system, SD, battery, Wi-Fi, tasks,
 *            uptime and last reset; refreshed every second (storage every 30 s:
 *            reading the free space of a big card is slow).
 *   Overlay  two small lines on top of every screen (RAM, PSRAM, battery,
 *            lowest RAM since it was turned on), updated every second. Turned on
 *            and off from the app, or with a double reset: press reset again
 *            within 4 seconds of the screen coming on. It stays on across restarts.
 */
#include "apps_internal.h"

#include "core/app.h"
#include "hardware/board.h"
#include "hardware/pmu.h"
#include "hardware/sdcard.h"
#include "core/sys.h"
#include "services/storage.h"
#include "services/wifi.h"
#include "ui/ui.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>

#define NS "sysmon"
#define DOUBLE_RESET_MS 4000

static lv_obj_t *s_overlay, *s_overlay_text;
static lv_timer_t *s_overlay_timer;
static size_t s_overlay_low;     /* lowest free internal RAM since the overlay is on */

static void kb(char *out, size_t size, size_t bytes)
{
    if (bytes >= 10 * 1024 * 1024)
    {
        snprintf(out, size, "%u.%u MB", (unsigned)(bytes >> 20), (unsigned)((bytes % (1 << 20)) * 10 >> 20));
    }
    else
    {
        snprintf(out, size, "%u KB", (unsigned)(bytes >> 10));
    }
}

static bool flag_get(const char *key)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v != 0;
}

static void flag_set(const char *key, bool on)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_u8(h, key, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------------------------------------------------------------- overlay */

static void overlay_tick(lv_timer_t *t)
{
    (void)t;
    size_t ram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (s_overlay_low == 0 || ram < s_overlay_low)
    {
        s_overlay_low = ram;
    }
    char text[96];
    snprintf(text, sizeof(text), "RAM %uK min %uK blk %uK\nPSRAM %uK  bat %d%%  task %u", (unsigned)(ram >> 10),
             (unsigned)(s_overlay_low >> 10), (unsigned)(block >> 10), (unsigned)(psram >> 10), pmu_battery_percent(),
             (unsigned)uxTaskGetNumberOfTasks());
    lv_label_set_text(s_overlay_text, text);
    lv_obj_set_style_text_color(s_overlay_text, ui_color(ram < 30 * 1024 ? 0xFF6B6B : ram < 60 * 1024 ? 0xFFC67B : 0xE3F4F5), 0);
}

/* LVGL context. */
static void overlay_show(bool on)
{
    if (on == (s_overlay != NULL))
    {
        return;
    }
    if (!on)
    {
        lv_timer_delete(s_overlay_timer);
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
        s_overlay_timer = NULL;
        return;
    }
    s_overlay_low = 0;
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_overlay, 8, 0);
    lv_obj_set_style_pad_hor(s_overlay, 8, 0);
    lv_obj_set_style_pad_ver(s_overlay, 3, 0);
    lv_obj_set_clickable(s_overlay, false);   /* touches go to the app below */
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_align(s_overlay, LV_ALIGN_TOP_MID, 0, board_info()->round ? 34 : 4);
    s_overlay_text = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_overlay_text, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_align(s_overlay_text, LV_TEXT_ALIGN_CENTER, 0);
    s_overlay_timer = lv_timer_create(overlay_tick, 1000, NULL);
    overlay_tick(NULL);
}

static void boot_window_closed(void *arg)
{
    (void)arg;
    flag_set("boot", false);   /* started long enough ago: the next reset is a single one */
}

void sistema_boot(void)
{
    bool double_reset = flag_get("boot");
    bool on = flag_get("overlay");
    if (double_reset)
    {
        on = !on;
        flag_set("overlay", on);
        flag_set("boot", false);
        ui_toast(on ? "Monitor di sistema acceso" : "Monitor di sistema spento");
    }
    else
    {
        flag_set("boot", true);
        static esp_timer_handle_t timer;
        const esp_timer_create_args_t args = {.callback = boot_window_closed, .name = "sysmon"};
        if (timer == NULL && esp_timer_create(&args, &timer) == ESP_OK)
        {
            esp_timer_start_once(timer, DOUBLE_RESET_MS * 1000ULL);
        }
    }
    overlay_show(on);
}

/* -------------------------------------------------------------------- app */

typedef struct
{
    lv_obj_t *ram, *psram, *flash, *sd, *battery, *wifi, *system;
    lv_timer_t *timer;
    int ticks;
} sistema_ui_t;

static sistema_ui_t *s_ui;

static void memory_text(lv_obj_t *l, uint32_t caps)
{
    char free_t[16], min_t[16], block_t[16], total_t[16], text[128];
    size_t total = heap_caps_get_total_size(caps), free_b = heap_caps_get_free_size(caps);
    kb(free_t, sizeof(free_t), free_b);
    kb(min_t, sizeof(min_t), heap_caps_get_minimum_free_size(caps));
    kb(block_t, sizeof(block_t), heap_caps_get_largest_free_block(caps));
    kb(total_t, sizeof(total_t), total);
    int used = total ? (int)((total - free_b) * 100 / total) : 0;
    snprintf(text, sizeof(text), "Libera %s su %s (%d%% usata)\nMinimo dall'avvio %s · blocco max %s", free_t, total_t, used,
             min_t, block_t);
    lv_label_set_text(l, text);
}

static void disk_text(lv_obj_t *l, const char *path, const char *missing)
{
    uint64_t total = 0, free_b = 0;
    char text[96], a[16], b[16];
    if (esp_vfs_fat_info(path, &total, &free_b) != ESP_OK || total == 0)
    {
        lv_label_set_text(l, missing);
        return;
    }
    kb(a, sizeof(a), (size_t)(free_b > SIZE_MAX ? SIZE_MAX : free_b));
    kb(b, sizeof(b), (size_t)(total > SIZE_MAX ? SIZE_MAX : total));
    if (total >= 4ULL << 30)
    {
        snprintf(text, sizeof(text), "Libera %.1f GB su %.1f GB (%d%% usata)", free_b / 1073741824.0, total / 1073741824.0,
                 (int)((total - free_b) * 100 / total));
    }
    else
    {
        snprintf(text, sizeof(text), "Libera %s su %s (%d%% usata)", a, b, (int)((total - free_b) * 100 / total));
    }
    lv_label_set_text(l, text);
}

static void refresh(lv_timer_t *t)
{
    sistema_ui_t *ui = lv_timer_get_user_data(t);
    char text[160];
    memory_text(ui->ram, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    memory_text(ui->psram, MALLOC_CAP_SPIRAM);
    if (ui->ticks++ % 30 == 0)
    {
        disk_text(ui->flash, STORAGE_FLASH_MOUNT, "Non montata");
        disk_text(ui->sd, SDCARD_MOUNT, "Nessuna scheda");
    }
    if (pmu_battery_present())
    {
        snprintf(text, sizeof(text), "%d%% · %d mV%s%s", pmu_battery_percent(), pmu_battery_mv(),
                 pmu_is_charging() ? " · in carica" : "", pmu_usb_present() ? " · USB" : "");
    }
    else
    {
        snprintf(text, sizeof(text), "Senza batteria%s", pmu_usb_present() ? " · USB" : "");
    }
    lv_label_set_text(ui->battery, text);
    if (wifi_is_connected())
    {
        char ssid[33] = "", ip[16] = "";
        wifi_current_ssid(ssid, sizeof(ssid));
        wifi_get_ip(ip, sizeof(ip));
        snprintf(text, sizeof(text), "%s · %d dBm\n%s", ssid, wifi_rssi(), ip);
    }
    else
    {
        snprintf(text, sizeof(text), "Spento (si accende quando serve)");
    }
    lv_label_set_text(ui->wifi, text);
    int64_t up = esp_timer_get_time() / 1000000;
    snprintf(text, sizeof(text), "Acceso da %lldg %02lld:%02lld:%02lld · %u task\nUltimo avvio: %s", up / 86400, up / 3600 % 24,
             up / 60 % 60, up % 60, (unsigned)uxTaskGetNumberOfTasks(), sys_last_reset_text());
    lv_label_set_text(ui->system, text);
}

static void toggle_overlay(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    flag_set("overlay", on);
    overlay_show(on);
}

static void deleted(lv_event_t *e)
{
    sistema_ui_t *ui = lv_event_get_user_data(e);
    if (ui->timer != NULL)
    {
        lv_timer_delete(ui->timer);
    }
    if (s_ui == ui)
    {
        s_ui = NULL;
    }
    free(ui);
}

static void create(lv_obj_t *screen, void *arg)
{
    (void)arg;
    lv_obj_t *page = ui_page(screen, "Sistema");
    sistema_ui_t *ui = calloc(1, sizeof(*ui));
    if (ui == NULL)
    {
        ui_text(page, "Memoria insufficiente", true);
        return;
    }
    s_ui = ui;
    lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, ui);
    ui_switch_row(page, LV_SYMBOL_EYE_OPEN, UI_COLOR_PURPLE, "Monitor leggero", s_overlay != NULL, toggle_overlay, NULL);
    ui_text(page, "Sempre in primo piano, anche nelle altre app. Si accende e spegne anche premendo reset due volte "
                  "(la seconda appena compare lo schermo, entro 4 secondi).", true);
    ui_section(page, "RAM INTERNA");
    ui->ram = ui_text(page, "", false);
    ui_section(page, "PSRAM");
    ui->psram = ui_text(page, "", false);
    ui_section(page, "MEMORIA INTERNA (FILE)");
    ui->flash = ui_text(page, "", false);
    ui_section(page, "SCHEDA SD");
    ui->sd = ui_text(page, "", false);
    ui_section(page, "BATTERIA");
    ui->battery = ui_text(page, "", false);
    ui_section(page, "WI-FI");
    ui->wifi = ui_text(page, "", false);
    ui_section(page, "SISTEMA");
    ui->system = ui_text(page, "", false);
    ui->timer = lv_timer_create(refresh, 1000, ui);
    if (ui->timer != NULL)
    {
        refresh(ui->timer);
    }
}

const app_t sistema_app = {
    .id = "sistema", .name = "Sistema", .icon = LV_SYMBOL_SETTINGS,
    .color = UI_COLOR_PURPLE, .create = create,
};
