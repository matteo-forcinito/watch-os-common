/*
 * Remote as a USB drive: the microSD appears on the PC, so the user can open the
 * "deck" folder and run the Watch Deck installer for their system by hand.
 * Nothing is typed or started on the PC by the watch.
 *
 * The ESP32-S3 has one USB port, used by the keyboard/mouse on the USB transport.
 * So the drive is a separate start of Remote: remote_drive_request() saves a flag
 * and restarts; at boot remote_drive_boot() sees it, shows only the drive screen
 * and hands the raw card to TinyUSB. "Fine" restarts into the normal Remote.
 * Boards without a native USB port (board_info()->has_usb_otg false) never get here.
 */
#include "remote.h"

#include "core/lv_port.h"
#include "core/power.h"
#include "extapp_sdk.h"
#include "hardware/board.h"
#include "hardware/sdcard.h"
#include "services/storage.h"
#include "ui/ui.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "nvs.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "remote_drive";

#define DRIVE_NS  "remote"
#define DRIVE_KEY "drive"

static lv_obj_t *s_status, *s_icon;
static volatile bool s_attached;
static tinyusb_msc_storage_handle_t s_storage;

bool remote_drive_supported(void)
{
    return board_info()->has_usb_otg;
}

int remote_drive_installers(void)
{
    static const char *const files[] = {"/sdcard/deck/WatchDeck.exe", "/sdcard/deck/WatchDeck-macos.zip",
                                        "/sdcard/deck/WatchDeck-linux.zip"};
    int found = 0;
    for (int i = 0; i < 3; i++)
    {
        struct stat info;
        if (stat(files[i], &info) == 0 && S_ISREG(info.st_mode))
            found |= 1 << i;
    }
    return found;
}

void remote_drive_request(void)
{
    nvs_handle_t handle;
    if (nvs_open(DRIVE_NS, NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_set_u8(handle, DRIVE_KEY, 1);
        nvs_commit(handle);
        nvs_close(handle);
    }
    extapp_restart_self(); /* the drive needs the USB port before the keyboard takes it */
}

static bool take_flag(void)
{
    nvs_handle_t handle;
    uint8_t value = 0;
    if (nvs_open(DRIVE_NS, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    if (nvs_get_u8(handle, DRIVE_KEY, &value) == ESP_OK && value)
    {
        nvs_erase_key(handle, DRIVE_KEY); /* one start only: a crash must not loop into the drive */
        nvs_commit(handle);
    }
    nvs_close(handle);
    return value != 0;
}

static void usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED)
        s_attached = true;
    else if (event->id == TINYUSB_EVENT_DETACHED)
        s_attached = false;
}

static const char *start_drive(void)
{
    if (!storage_files_acquire())
        return "Scheda SD occupata";
    sdmmc_card_t *card = NULL;
    if (sdcard_open_raw(&card) != ESP_OK)
    {
        storage_files_release();
        return "Scheda SD non trovata";
    }
    tinyusb_msc_driver_config_t msc_config = {.user_flags.auto_mount_off = 1};
    if (tinyusb_msc_install_driver(&msc_config) != ESP_OK)
        return "Errore USB (MSC)";
    tinyusb_msc_storage_config_t storage_config = {
        .medium.card = card,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {.do_not_format = true},
    };
    if (tinyusb_msc_new_storage_sdmmc(&storage_config, &s_storage) != ESP_OK)
        return "Errore USB (scheda)";
    tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG(usb_event);
    if (tinyusb_driver_install(&usb_config) != ESP_OK)
        return "Errore USB";
    return NULL;
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    lv_label_set_text(s_status, s_attached
                                    ? "Sul PC si apre l'unità della microSD.\nCartella deck: avvia l'installer del "
                                      "tuo sistema.\nPoi espelli l'unità e tocca Fine."
                                    : "Collega il cavo USB al PC.");
    ui_icon_bubble_set_color(s_icon, s_attached ? UI_COLOR_GREEN : UI_COLOR_BLUE);
}

static void finish(lv_event_t *e)
{
    (void)e;
    extapp_restart_self(); /* back to the normal Remote */
}

bool remote_drive_boot(void)
{
    if (!take_flag())
        return false;
    ESP_LOGI(TAG, "USB drive start");
    /* No light sleep and full speed while the PC reads the card (a slow drive stalls Explorer). */
    esp_pm_lock_handle_t sleep_lock = NULL, cpu_lock = NULL;
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "remote_drive", &sleep_lock) == ESP_OK)
        esp_pm_lock_acquire(sleep_lock);
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "remote_drive_cpu", &cpu_lock) == ESP_OK)
        esp_pm_lock_acquire(cpu_lock);

    lv_port_lock();
    ui_init();
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(screen, 20, 0);
    lv_obj_set_style_pad_row(screen, 12, 0);
    s_icon = ui_icon_bubble(screen, LV_SYMBOL_USB, UI_COLOR_BLUE, 72);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Installa Watch Deck");
    lv_obj_set_style_text_font(title, ui_font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    s_status = lv_label_create(screen);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    const char *error = start_drive();
    if (error != NULL)
    {
        ESP_LOGE(TAG, "%s", error);
        lv_label_set_text(s_status, error);
        lv_obj_set_style_text_color(s_status, lv_color_hex(UI_COLOR_RED), 0);
    }
    else
    {
        lv_timer_create(tick, 500, NULL);
        tick(NULL);
    }
    ui_button(screen, "Fine", UI_COLOR_BLUE, finish, NULL);
    lv_screen_load(screen);
    lv_port_unlock();
    return true;
}
