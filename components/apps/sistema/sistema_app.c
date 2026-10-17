/*
 * Sistema: the state of the device, and a light overlay to watch it under load.
 *
 *   HOME     like Bilancio: outer arc = internal RAM used, inner arc (touching)
 *            = PSRAM used; free RAM in big digits, PSRAM, lowest, battery;
 *            round buttons: details, light monitor on/off.
 *   DETAILS  RAM, PSRAM, file system, SD (every 30 s: slow on big cards),
 *            battery, Wi-Fi, tasks, uptime, last reset.
 *   Overlay  two small lines on top of every screen (RAM, PSRAM, battery,
 *            lowest RAM since it was turned on), updated every second. Turned on
 *            and off from the app, or with a double reset: press reset again
 *            within 10 seconds of the screen coming on. It stays on across restarts.
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

#include "core/lv_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>

#define NS "sysmon"
#define DOUBLE_RESET_MS 10000

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
    /* The system layer: above everything, and the focus logic ignores it (the top layer is for dialogs). */
    s_overlay = lv_obj_create(lv_layer_sys());
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
    ESP_LOGI("sysmon", "overlay %s (double reset %s)", on ? "on" : "off", double_reset ? "yes" : "no");
    overlay_show(on);
}

/* -------------------------------------------------------------------- app */

#define C_BG   0x060B12
#define C_CARD 0x111A26
#define C_LINE 0x1F2C3D
#define C_INK  0xE3F4F5
#define C_DIM  0x8A9BB0

enum { HOME, DETAILS };

typedef struct
{
    lv_obj_t *screen, *content;
    int view;
    lv_obj_t *ram_arc, *psram_arc, *big, *sub, *info, *eye;   /* HOME */
    lv_obj_t *ram, *psram, *flash, *sd, *battery, *wifi, *system;   /* DETAILS */
    lv_timer_t *timer;
    int ticks;
} sistema_ui_t;

static sistema_ui_t *s_ui;
static void render(sistema_ui_t *ui);

static uint32_t level_color(int used)
{
    return used >= 90 ? 0xFF6B6B : used >= 75 ? 0xFFA34D : 0x2EE39A;
}

static int used_pct(uint32_t caps)
{
    size_t total = heap_caps_get_total_size(caps);
    return total ? (int)((total - heap_caps_get_free_size(caps)) * 100 / total) : 0;
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_color(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *gauge(lv_obj_t *parent, int size, int width)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, ui_color(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_clickable(arc, false);
    return arc;
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 52, 52);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, ui_color(C_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, ui_color(C_LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_bg_color(b, ui_color(C_LINE), LV_STATE_PRESSED);
    lv_obj_t *l = text(b, UI_FONT_LARGE, color);
    lv_label_set_text(l, symbol);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_port_set_focusable(b);
    return b;
}

static void home_update(sistema_ui_t *ui)
{
    uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    int ram = used_pct(internal), psram = used_pct(MALLOC_CAP_SPIRAM);
    lv_arc_set_value(ui->ram_arc, ram);
    lv_obj_set_style_arc_color(ui->ram_arc, ui_color(level_color(ram)), LV_PART_INDICATOR);
    lv_arc_set_value(ui->psram_arc, psram);
    lv_obj_set_style_arc_color(ui->psram_arc, ui_color(psram >= 90 ? 0xFF6B6B : 0x4DA3FF), LV_PART_INDICATOR);
    char t[64], a[16];
    kb(a, sizeof(a), heap_caps_get_free_size(internal));
    lv_label_set_text(ui->big, a);
    kb(a, sizeof(a), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    snprintf(t, sizeof(t), "RAM libera · PSRAM %s", a);
    lv_label_set_text(ui->sub, t);
    kb(a, sizeof(a), heap_caps_get_minimum_free_size(internal));
    if (pmu_battery_present())
    {
        snprintf(t, sizeof(t), "minimo %s · batteria %d%%%s", a, pmu_battery_percent(), pmu_is_charging() ? "+" : "");
    }
    else
    {
        snprintf(t, sizeof(t), "minimo %s", a);
    }
    lv_label_set_text(ui->info, t);
    lv_obj_set_style_text_color(lv_obj_get_child(ui->eye, 0), ui_color(s_overlay != NULL ? 0x2EE39A : C_DIM), 0);
}

static void open_details(lv_event_t *e)
{
    (void)e;
    s_ui->view = DETAILS;
    render(s_ui);
}

static void toggle_overlay(lv_event_t *e)
{
    (void)e;
    bool on = s_overlay == NULL;
    flag_set("overlay", on);
    overlay_show(on);
    ui_toast(on ? "Monitor leggero acceso" : "Monitor leggero spento");
    home_update(s_ui);
}

static void home(sistema_ui_t *ui)
{
    const board_info_t *b = board_info();
    int size = b->round ? (b->width < b->height ? b->width : b->height) - 20 : b->width - 70;
    int width = size / 16;
    ui->ram_arc = gauge(ui->content, size, width);
    lv_obj_align(ui->ram_arc, b->round ? LV_ALIGN_CENTER : LV_ALIGN_TOP_MID, 0, b->round ? 0 : 6);
    ui->psram_arc = gauge(ui->content, size - 2 * width, width);   /* touching the outer one */
    lv_obj_align_to(ui->psram_arc, ui->ram_arc, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *col = box(ui->content);
    lv_obj_set_size(col, size * 56 / 100, LV_SIZE_CONTENT);
    lv_obj_align_to(col, ui->ram_arc, LV_ALIGN_CENTER, 0, -size / 16);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 1, 0);
    lv_label_set_text(text(col, UI_FONT_SMALL, 0x2EE39A), "RAM · " LV_SYMBOL_BULLET " PSRAM");
    ui->big = text(col, ui_font_big, C_INK);
    ui->sub = text(col, UI_FONT_SMALL, C_DIM);
    ui->info = text(col, UI_FONT_SMALL, 0x5C6B80);

    lv_obj_t *buttons = box(ui->content);
    lv_obj_set_size(buttons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 10, 0);
    round_button(buttons, LV_SYMBOL_LIST, 0x4DA3FF, open_details);
    ui->eye = round_button(buttons, LV_SYMBOL_EYE_OPEN, C_DIM, toggle_overlay);
    lv_obj_align(buttons, LV_ALIGN_BOTTOM_MID, 0, -10);
    home_update(ui);
}

static void memory_text(lv_obj_t *l, uint32_t caps)
{
    char free_t[16], min_t[16], block_t[16], total_t[16], t[128];
    kb(free_t, sizeof(free_t), heap_caps_get_free_size(caps));
    kb(min_t, sizeof(min_t), heap_caps_get_minimum_free_size(caps));
    kb(block_t, sizeof(block_t), heap_caps_get_largest_free_block(caps));
    kb(total_t, sizeof(total_t), heap_caps_get_total_size(caps));
    snprintf(t, sizeof(t), "%s su %s · %d%%\nminimo %s · blocco %s", free_t, total_t, used_pct(caps), min_t, block_t);
    lv_label_set_text(l, t);
}

static void disk_text(lv_obj_t *l, const char *path, const char *missing)
{
    uint64_t total = 0, free_b = 0;
    char t[96];
    if (esp_vfs_fat_info(path, &total, &free_b) != ESP_OK || total == 0)
    {
        lv_label_set_text(l, missing);
        return;
    }
    snprintf(t, sizeof(t), "%.1f MB liberi su %.1f MB · %d%%", free_b / 1048576.0, total / 1048576.0,
             (int)((total - free_b) * 100 / total));
    lv_label_set_text(l, t);
}

static void details_update(sistema_ui_t *ui)
{
    char t[160];
    memory_text(ui->ram, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    memory_text(ui->psram, MALLOC_CAP_SPIRAM);
    if (ui->ticks % 30 == 0)
    {
        disk_text(ui->flash, STORAGE_FLASH_MOUNT, "Non montata");
        disk_text(ui->sd, SDCARD_MOUNT, "Nessuna scheda");
    }
    if (pmu_battery_present())
    {
        snprintf(t, sizeof(t), "%d%% · %d mV%s%s", pmu_battery_percent(), pmu_battery_mv(),
                 pmu_is_charging() ? " · in carica" : "", pmu_usb_present() ? " · USB" : "");
    }
    else
    {
        snprintf(t, sizeof(t), "Senza batteria%s", pmu_usb_present() ? " · USB" : "");
    }
    lv_label_set_text(ui->battery, t);
    if (wifi_is_connected())
    {
        char ssid[33] = "", ip[16] = "";
        wifi_current_ssid(ssid, sizeof(ssid));
        wifi_get_ip(ip, sizeof(ip));
        snprintf(t, sizeof(t), "%s · %d dBm\n%s", ssid, wifi_rssi(), ip);
    }
    else
    {
        snprintf(t, sizeof(t), "Spento (si accende quando serve)");
    }
    lv_label_set_text(ui->wifi, t);
    int64_t up = esp_timer_get_time() / 1000000;
    snprintf(t, sizeof(t), "Acceso da %lldg %02lld:%02lld:%02lld · %u task\nUltimo avvio: %s", up / 86400, up / 3600 % 24,
             up / 60 % 60, up % 60, (unsigned)uxTaskGetNumberOfTasks(), sys_last_reset_text());
    lv_label_set_text(ui->system, t);
}

static void details(sistema_ui_t *ui)
{
    lv_obj_t *page = ui_page(ui->content, "Dettagli");
    ui_section(page, "RAM INTERNA");
    ui->ram = ui_text(page, "", false);
    ui_section(page, "PSRAM");
    ui->psram = ui_text(page, "", false);
    ui_section(page, "MEMORIA FILE");
    ui->flash = ui_text(page, "", false);
    ui_section(page, "SCHEDA SD");
    ui->sd = ui_text(page, "", false);
    ui_section(page, "BATTERIA");
    ui->battery = ui_text(page, "", false);
    ui_section(page, "WI-FI");
    ui->wifi = ui_text(page, "", false);
    ui_section(page, "SISTEMA");
    ui->system = ui_text(page, "", false);
    ui_text(page, "Monitor leggero: occhio nella schermata principale, oppure reset due volte "
                  "(la seconda entro 10 secondi da quando compare lo schermo).", true);
    ui->ticks = 0;
    details_update(ui);
}

static void render(sistema_ui_t *ui)
{
    if (ui->content != NULL)
    {
        lv_obj_delete(ui->content);
    }
    ui->content = box(ui->screen);
    lv_obj_set_size(ui->content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui->content, ui_color(C_BG), 0);
    lv_obj_set_style_bg_opa(ui->content, LV_OPA_COVER, 0);
    ui->ram_arc = NULL;
    ui->ram = NULL;
    if (ui->view == HOME)
    {
        home(ui);
    }
    else
    {
        details(ui);
    }
}

static void refresh(lv_timer_t *t)
{
    sistema_ui_t *ui = lv_timer_get_user_data(t);
    ui->ticks++;
    if (ui->ram_arc != NULL)
    {
        home_update(ui);
    }
    else if (ui->ram != NULL)
    {
        details_update(ui);
    }
}

static bool back(void)
{
    if (s_ui == NULL || s_ui->view == HOME)
    {
        return false;
    }
    s_ui->view = HOME;
    render(s_ui);
    return true;
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
    sistema_ui_t *ui = calloc(1, sizeof(*ui));
    if (ui == NULL)
    {
        ui_text(ui_page(screen, "Sistema"), "Memoria insufficiente", true);
        return;
    }
    s_ui = ui;
    ui->screen = screen;
    lv_obj_set_style_bg_color(screen, ui_color(C_BG), 0);
    lv_obj_add_event_cb(screen, deleted, LV_EVENT_DELETE, ui);
    render(ui);
    ui->timer = lv_timer_create(refresh, 1000, ui);
}

const app_t sistema_app = {
    .id = "sistema", .name = "Sistema", .icon = LV_SYMBOL_SETTINGS,
    .color = UI_COLOR_PURPLE, .create = create, .back = back,
};
